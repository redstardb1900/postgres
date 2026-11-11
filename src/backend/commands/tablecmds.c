/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *	  用于创建和修改表结构与设置的命令                
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
 * ON COMMIT 动作集  
 */
typedef struct OnCommitItem
{
	Oid			relid;			/* 本关系的 relid*/
	OnCommitAction oncommit;	/* xact 结束时要做什么*/

	/*
	 * 如果本条目创建于当前事务，creating_subid    
	 * 为创建它的子事务的 ID；如果是在先前事务中创建，  
	 * 则 creating_subid 为零。若在当前事务中被删除，      
	 * 则 deleting_subid 为执行删除子事务的 ID；若无待       
	 * 处理的删除请求，deleting_subid 为零。    
	 */
	SubTransactionId creating_subid;
	SubTransactionId deleting_subid;
} OnCommitItem;

static List *on_commits = NIL;


/*
 * ALTER TABLE 的状态信息      
 *
 * ALTER TABLE 的待处理工作队列是一个 AlteredTableInfo 结构  
 * 构体的列表，被操作修改的每张表（命名表加上受    
 * 影响的子表）各一个。我们保存子命令的列表，        
 * 以便应用到该表（可能被语法解析转换步骤修改）；    
 * 这些列表将在阶段 2 执行。如果需要阶段 3 步骤，    
 * 相关信息会保存在 constraints 和 newvals 列表中。        
 *
 * 阶段 2 被划分为多个趟；子命令在由子命令类型所  
 * 决定的趟中执行。             
 */

#define AT_PASS_UNSET			-1	/* UNSET 将导致 ERROR  */
#define AT_PASS_DROP			0	/* DROP (所有变体)*/
#define AT_PASS_ALTER_TYPE		1	/* 更改列类型   */
#define AT_PASS_OLD_INDEX		2	/* 重建已存在的索引*/
#define AT_PASS_OLD_CONSTR		3	/* 重建已存在的约束    */
/* 此处本可支持一个 RENAME COLUMN 趟，但目前未使用    */
#define AT_PASS_ADD_COL			4	/* 添加列  */
#define AT_PASS_ADD_CONSTR		5	/* ADD 约束（初步检查）          */
#define AT_PASS_COL_ATTRS		6	/* 设置列属性，例如 NOT NULL  */
#define AT_PASS_ADD_INDEXCONSTR	7	/* ADD 基于索引的约束   */
#define AT_PASS_ADD_INDEX		8	/* ADD 索引  */
#define AT_PASS_ADD_OTHERCONSTR	9	/* ADD 其他约束与默认值    */
#define AT_PASS_MISC			10	/* 其他事项*/
#define AT_NUM_PASSES			11

typedef struct AlteredTableInfo
{
	/* 在任何工作开始之前保存的信息：*/
	Oid			relid;			/* 待处理的关系  */
	char		relkind;		/* 其 relkind */
	TupleDesc	oldDesc;		/* 修改前的元组描述符       */

	/*
	 * 阶段 2 期间临时设置，通常设为 NULL。   
	 *
	 * ATRewriteCatalogs 在启动时设置它，并在 ATExecCmd           
	 * 交还控制时关闭。ATExecCmd 的子例程可借此            
	 * 跨事务边界关闭/重新打开。      
	 */
	Relation	rel;

	/* 阶段 1 为阶段 2 保存的信息：   */
	List	   *subcmds[AT_NUM_PASSES]; /* AlterTableCmd 列表   */
	/* 阶段 1/2 为阶段 3 保存的信息：    */
	List	   *constraints;	/* NewConstraint 列表  */
	List	   *newvals;		/* NewColumnValue 列表  */
	List	   *afterStmts;		/* 工具命令语法树的列表     */
	bool		verify_new_notnull; /* T 表示应重新检查 NOT NULL*/
	int			rewrite;		/* 强制重写的原因（如有） */
	Oid			newAccessMethod;	/* 新访问方法；0 表示无变化  */
	Oid			newTableSpace;	/* 新表空间；0 表示无变化  */
	bool		chgPersistence; /* 用 SET LOGGED/UNLOGGED 时为 T */
	char		newrelpersistence;	/* 若上述为真  */
	Expr	   *partition_constraint;	/* 用于附加分区的验证     */
	/* 若因其他附加/分离而验证默认分区，则为真  */
	bool		validate_default;
	/* 完成 ALTER TYPE 操作之后需要重建的对象       */
	List	   *changedConstraintOids;	/* 需要重建的约束的 OID   */
	List	   *changedConstraintDefs;	/* 同样的字符串定义   */
	List	   *changedIndexOids;	/* 需要重建的索引 OID  */
	List	   *changedIndexDefs;	/* 同样的字符串定义   */
	char	   *replicaIdentityIndex;	/* REPLICA IDENTITY 重置用索引   */
	char	   *clusterOnIndex; /* 用于 CLUSTER 的索引 */
	List	   *changedStatisticsOids;	/* 需重建的统计信息 OID  */
	List	   *changedStatisticsDefs;	/* 同样的字符串定义   */
} AlteredTableInfo;

/* 描述阶段 3 扫描中要检查的新约束的结构体     */
/* 注意：新的 NOT NULL 约束在其他地方处理 */
typedef struct NewConstraint
{
	char	   *name;			/* 约束名，若无则为 NULL    */
	ConstrType	contype;		/* CHECK 或 FOREIGN*/
	Oid			refrelid;		/* 外键的 PK 关系*/
	Oid			refindid;		/* FOREIGN 时 PK 索引的 OID  */
	Oid			conid;			/* FOREIGN 时 pg_constraint 条目的 OID*/
	Node	   *qual;			/* 检查表达式或 CONSTR_FOREIGN       */
	ExprState  *qualstate;		/* CHECK 表达式的执行状态 */
} NewConstraint;

/*
 * 描述需要在阶段 3 拷贝期间计算的新列值的结构体     
 * （它可能是一个带有非空默认值的新列，也可能是         
 * 一个正在被我们改变类型的列）。没有这种条目的列    
 * 在 ATRewriteTable 期间只会从旧表中原样拷贝。注意        
 * expr 是一个基于*旧*表值的表达式，除非 is_generated      
 * 为真；此时它是基于*新*元组各列的表达式。       
 */
typedef struct NewColumnValue
{
	AttrNumber	attnum;			/* 哪一列    */
	Expr	   *expr;			/* 要计算的表达式 */
	ExprState  *exprstate;		/* 执行状态    */
	bool		is_generated;	/* 是 GENERATED 表达式吗    */
} NewColumnValue;

/*
 * RemoveRelations 的错误报告支持      
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

/* RemoveRelations 与 RangeVarCallbackForDropRelation 之间的通信       */
struct DropRelationCallbackState
{
	/* 这些字段由 RemoveRelations 设置：*/
	char		expected_relkind;
	LOCKMODE	heap_lockmode;
	/* 这些字段是用于跟踪持有哪些从属锁的状态：     */
	Oid			heapOid;
	Oid			partParentOid;
	/* 这些字段由 RangeVarCallbackForDropRelation 传回：        */
	char		actual_relkind;
	char		actual_relpersistence;
};

/* ATSimplePermissions 的 ALTER TABLE 目标类型标志*/
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
 * 与外部表截断有关的信息。用作哈希表中的元素。    
 * 它使用服务器的 OID 作为查找键，                       
 * 并包含参与截断的所有外部表的一个按服务器分组  
 * 列表。  
 */
typedef struct ForeignTruncateInfo
{
	Oid			serverid;
	List	   *rels;
} ForeignTruncateInfo;

/* addFkConstraint() 中部分或完整的外键创建  */
typedef enum addFkConstraintSides
{
	addFkReferencedSide,
	addFkReferencingSide,
	addFkBothSides,
} addFkConstraintSides;

/*
 * 当父分区表被删除时，分区表预期也将随之被删除。  
 * 因此对于分区我们使用 AUTO 依赖。                     
 * 否则，对于普通继承使用 NORMAL 依赖。       
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
 *				创建新关系。     
 *
 * stmt 携带来自普通 CREATE TABLE 语句的语法树信息。           
 * 其余参数用于针对其他情形扩展其行为：              
 * relkind: 赋给新关系的 relkind           
 * ownerId: 若非 InvalidOid，则用作新关系的属主。      
 * typaddress: 若非空，设为 pg_type 条目的地址。        
 * queryString: 用于错误报告 
 *
 * 注意，无论 ownerId 为何，权限检查始终针对当前用户   
 * 进行。非零的 ownerId 用于有人替他人创建关系之时，  
 * 因此我们仍要确认当前用户具有执行此操作的权限。     
 *                          
 *
 * 如果成功，返回新关系的地址。             
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
	 * 将 relname 截断为适当的长度（这可能是浪费时间，  
	 * 语法分析器本应已做过）。  
	 */
	strlcpy(relname, stmt->relation->relname, NAMEDATALEN);

	/*
	 * 检查参数的一致性      
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
	 * 查找我们应当在其下创建关系的命名空间，检查我们 
	 * 是否具有创建权限，将它锁住以防并发删除，        
	 * 若选中了临时命名空间，则将 stmt->relation 标记为   
	 * RELPERSISTENCE_TEMP   
	 */
	namespaceId =
		RangeVarGetAndCheckCreationNamespace(stmt->relation, NoLock, NULL);

	/*
	 * 安全检查：不允许在安全受限代码中创建临时表。    
	 * 这样做是必要的，因为调用方代码可能不期望在      
	 * 其搜索路径前端的 pg_temp 中出现不可信表。   
	 */
	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP
		&& InSecurityRestrictedOperation())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("cannot create temporary table within security-restricted operation")));

	/*
	 * 确定扫描父表时要使用的锁模式。这里需要一把       
	 * 自身独占锁。  
	 *
	 * 对于普通继承，如果两个后端同时尝试向同一个父表  
	 * 添加子表，而该父表没有先前存在的子表，      
	 * 那么两者都会尝试更新父表的 relhassubclass 字段，      
	 * 从而导致 "tuple concurrently updated" 错误。此外，       
	 * 它还能与父表上的并发 ANALYZE 互锁，后者            
	 * 若其先前的子表最近被删除，可能正试图清除      
	 * 父表的 relhassubclass 字段。                    
	 *
	 * 如果子表是一个分区，那么我们改为在父表上获取  
	 * 排他锁，因为它的分区描述符将因新分区的加入而    
	 * 被改变。                  
	 */
	parentLockmode = (stmt->partbound != NULL ? AccessExclusiveLock :
					  ShareUpdateExclusiveLock);

	/* 确定父表的 OID 列表。              */
	inheritOids = NIL;
	foreach(listptr, stmt->inhRelations)
	{
		RangeVar   *rv = (RangeVar *) lfirst(listptr);
		Oid			parentOid;

		parentOid = RangeVarGetRelid(rv, parentLockmode, false);

		/*
		 * 拒绝父表列表中的重复项。       
		 */
		if (list_member_oid(inheritOids, parentOid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" would be inherited from more than once",
							get_rel_name(parentOid))));

		inheritOids = lappend_oid(inheritOids, parentOid);
	}

	/*
	 * 选择要使用的表空间：显式指定的那一个，或者（在 
	 * 分区表）取父表的表空间，若它有。    
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

	/* 除使用数据库默认表空间外都要检查权限 */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   get_tablespace_name(tablespaceId));
	}

	/* 任何情况下都不允许将用户关系放入 pg_global*/
	if (tablespaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/* 确定将拥有该表的用户 ID        */
	if (!OidIsValid(ownerId))
		ownerId = GetUserId();

	/*
	 * 如有 reloptions 则解析验证。  
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
	 * 查找继承祖先，生成包含继承属性的关系模式。      
	 * （注意 stmt->tableElts 会被 MergeAttributes 破坏性地     
	 * 修改。）                 
	 */
	stmt->tableElts =
		MergeAttributes(stmt->tableElts, inheritOids,
						stmt->relation->relpersistence,
						stmt->partbound != NULL,
						&old_constraints);

	/*
	 * 从关系模式创建元组描述符。注意它处理的是列    
	 * 名、类型和 NOT NULL 约束，但不包括默认值和       
	 * CHECK 约束；这些我们将在下面处理。           
	 */
	descriptor = BuildDescForRelation(stmt->tableElts);

	/*
	 * 找出带有默认值的列并准备插入这些默认值。     
	 * 预处理的（即继承而来的）默认值进入一个            
	 * CookedConstraint 结构体列表，我们会将其传递给          
	 * heap_create_with_catalog；原始默认值则进入 RawColumnDefault   
	 * 结构体的列表，交给 AddRelationNewConstraints 处理（    
	 * transformExpr 可用前无法处理。）  
	 *
	 * 现在就可以在元组描述符中设置 atthasdef 标志；       
	 * 这可让 StoreAttrDefault 免于立即更新这些 pg_attribute   
	 * 行。            
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
			cooked->conoid = InvalidOid;	/* 直到创建  */
			cooked->name = NULL;
			cooked->attnum = attnum;
			cooked->expr = colDef->cooked_default;
			cooked->skip_validation = false;
			cooked->is_local = true;	/* 不用于默认值    */
			cooked->inhcount = 0;	/* 同上*/
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
	 * 如果语句没有指定访问方法，但我们正在定义的       
	 * 关系类型需要它，则使用默认值。      
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

	/* 查找该访问方法，验证它是用于表的    */
	if (accessMethod != NULL)
		accessMethodId = get_table_am_oid(accessMethod, false);

	/*
	 * 创建该关系。继承的默认值和约束会被传入以便       
	 * 立即处理 --- 因为它们不需要解析，所以可以被       
	 * 立即存储。    
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
	 * 我们必须递增命令计数器，使新创建的关系元组    
	 * 对于打开可见。     
	 */
	CommandCounterIncrement();

	/*
	 * 打开新关系并获取其排他锁。这对阻挡其他后端    
	 * 并非真正必要（因为在我们提交之前它们反正也      
	 * 看不到新关系），但它能让锁管理器不因                 
	 * 死锁风险而抱怨。         
	 */
	rel = relation_open(relationId, AccessExclusiveLock);

	/*
	 * 现在将任何新指定的列默认值和生成表达式加入      
	 * 新关系。它们以原始语法树形式传给我们；      
	 * 我们需要先把它们转换成可执行的表达式树，然后  
	 * 才能加入它们。最方便的办法是应用解析器的      
	 * transformExpr 例程，但如果我们没有一个已存在的     
	 * 关系，transformExpr 就无法工作。因此，这一转换必须   
	 * 被推迟到 CREATE TABLE 的这最后一步。    
	 *
	 * 这必须放在处理分区子句之前，因为它们可能       
	 * 引用生成列。                     
	 */
	if (rawDefaults)
		AddRelationNewConstraints(rel, rawDefaults, NIL,
								  true, true, false, queryString);

	/*
	 * 使列生成表达式对于分区处理可见可用。             
	 */
	CommandCounterIncrement();

	/* 处理并存储分区边界，如有。    */
	if (stmt->partbound)
	{
		PartitionBoundSpec *bound;
		ParseState *pstate;
		Oid			parentId = linitial_oid(inheritOids),
					defaultPartOid;
		Relation	parent,
					defaultRel = NULL;
		ParseNamespaceItem *nsitem;

		/* 在父表上已持有足够强的锁          */
		parent = table_open(parentId, NoLock);

		/*
		 * 我们将试着用 parentRel 的分区键来验证分区边界   
		 * 规格说明，所以它最好有分区键。                 
		 */
		if (parent->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("\"%s\" is not partitioned",
							RelationGetRelationName(parent))));

		/*
		 * 默认分区的分区约束依赖于所有其他分区的分区 
		 * 边界。可能另一个后端即将在默认分区表上     
		 * 执行查询，且该查询依赖先前已缓存的默认       
		 * 分区约束。因此必须拿一把足够强的表锁，      
		 * 以便在提交并发送共享缓存失效通知之前，         
		 * 阻止所有针对默认分区的查询继续进行；            
		 * 该通知会让它们更新各自的                               
		 * 索引列表。                              
		 *
		 * 加锁顺序：新关系在提交之前对其他后端不可  
		 * 见，所以在 DefineRelation() 中               
		 * 先锁默认分区还是先锁待添加关系并不重要。       
		 * 但在其他所有地方，我们需要在锁定被添加或    
		 * 被移除的关系之前先锁定默认关系，即所有地方    
		 * 都应按相同的顺序获取锁：先锁父表，               
		 * 再锁默认分区，最后锁定该分区，                    
		 * 从而避免死锁。                        
		 */
		defaultPartOid =
			get_default_oid_from_partdesc(RelationGetPartitionDesc(parent,
																   true));
		if (OidIsValid(defaultPartOid))
			defaultRel = table_open(defaultPartOid, AccessExclusiveLock);

		/* 转换边界值            */
		pstate = make_parsestate(NULL);
		pstate->p_sourcetext = queryString;

		/*
		 * 添加含此关系的 nsitem，让分区边界表达式      
		 * 调用的 transformExpr 能以恰当的上下文报错。     
		 *                        
		 */
		nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
											   NULL, false, false);
		addNSItemToQuery(pstate, nsitem, false, true, true);

		bound = transformPartitionBound(pstate, parent, stmt->partbound);

		/*
		 * 先检查新分区的边界有效且不与父表任何现存    
		 * 分区重叠。                                       
		 */
		check_new_partition_bound(relname, parent, bound, pstate);

		/*
		 * 如果默认分区存在，其分区约束在加入此新分   
		 * 区之后发生改变，以致不再允许任何符合这个新   
		 * 分区的行。因此要检查默认分区的现有数据          
		 * 是否满足加入此分区之后将存在的那个约束。       
		 *                                              
		 */
		if (OidIsValid(defaultPartOid))
		{
			check_default_partition_contents(parent, defaultRel, bound);
			/* 持有该锁直到提交。 */
			table_close(defaultRel, NoLock);
		}

		/* 更新 pg_class 条目。  */
		StorePartitionBound(rel, parent, bound);

		table_close(parent, NoLock);
	}

	/* 为新关系存储继承信息。          */
	StoreCatalogInheritance(relationId, inheritOids, stmt->partbound != NULL);

	/*
	 * 处理分区规格说明（如果有），并把分区键信息        
	 * 存储到系统表中。         
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

		/* 保护此处与执行器的固定大小数组  */
		if (partnatts > PARTITION_MAX_KEYS)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("cannot partition using more than %d columns",
							PARTITION_MAX_KEYS)));

		/*
		 * 我们需要把与分区表达式对应的原始语法树转换   
		 * 成可执行的表达式树。与列默认值、CHECK 约束       
		 * 一样，这一转换无法更早完成                         
		 * 。     
		 */
		stmt->partspec = transformPartitionSpec(rel, stmt->partspec);

		ComputePartitionAttrs(pstate, rel, stmt->partspec->partParams,
							  partattrs, &partexprs, partopclass,
							  partcollation, stmt->partspec->strategy);

		StorePartitionKey(rel, stmt->partspec->strategy, partnatts, partattrs,
						  partexprs,
						  partopclass, partcollation);

		/* 使它全部可见  */
		CommandCounterIncrement();
	}

	/*
	 * 如果创建分区，则现在创建父表定义的全部索引、  
	 * 触发器和外键。     
	 *
	 * 我们无法更早做到这一点，因为 DefineIndex 想知道       
	 * 刚存储的分区键。 
	 */
	if (stmt->partbound)
	{
		Oid			parentId = linitial_oid(inheritOids);
		Relation	parent;
		List	   *idxlist;
		ListCell   *cell;

		/* 在父表上已持有足够强的锁          */
		parent = table_open(parentId, NoLock);
		idxlist = RelationGetIndexList(parent);

		/*
		 * 对于父表中的每个索引，在分区中创建一个。   
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
		 * 若有任何行级触发器，将它们克隆到新       
		 * 分区。 
		 */
		if (parent->trigdesc != NULL)
			CloneRowTriggersToPartition(parent, rel);

		/*
		 * 外键也是如此。注意因为我们是全新创建该表，    
		 * 因此没有必要验证这些新约束。              
		 */
		CloneForeignKeyConstraints(NULL, parent, rel);

		table_close(parent, NoLock);
	}

	/*
	 * 现在把任何新指定的 CHECK 约束加入新关系。与上面    
	 * 的默认值处理相同，但这些必须等到分区设置完毕。  
	 *    
	 */
	if (stmt->constraints)
		AddRelationNewConstraints(rel, NIL, stmt->constraints,
								  true, true, false, queryString);

	ObjectAddressSet(address, RelationRelationId, relationId);

	/*
	 * 清理。我们保留新关系上的锁（尽管提交之前它  
	 * 本来别人也不会看到它）。         
	 */
	relation_close(rel, NoLock);

	return address;
}

/*
 * 为对不存在的关系发出的 "DROP" 命令给出正确的错误    
 * 或警告消息。   
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

	Assert(rentry->kind != '\0');	/* 应该不可能发生*/
}

/*
 * 为对类型错误的关系发出的 "DROP" 命令给出     
 * 正确的错误消息。  
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
	/* wrongkind 可能是表中所没有的值……             */

	ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg(rentry->nota_msg, relname),
			 (wentry->kind != '\0') ? errhint("%s", _(wentry->drophint_msg)) : 0));
}

/*
 * RemoveRelations
 *		实现 DROP TABLE,DROP INDEX,DROP SEQUENCE,DROP VIEW        
 *		DROP MATERIALIZED VIEW,DROP FOREIGN TABLE 
 */
void
RemoveRelations(DropStmt *drop)
{
	ObjectAddresses *objects;
	char		relkind;
	ListCell   *cell;
	int			flags = 0;
	LOCKMODE	lockmode = AccessExclusiveLock;

	/* DROP CONCURRENTLY 使用较弱的锁，并且有一些限制    */
	if (drop->concurrent)
	{
		/*
		 * 注意，对临时关系而言这把锁以后可能被升级，   
		 * 但由于没有其他会话能访问临时关系，其实没有  
		 * 问题。        
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
	 * 我们先标识出所有关系，然后在单独一次              
	 * performMultipleDeletions() 调用中删除它们，以免       
	 * 依赖其他关系时出现 DROP RESTRICT 错误。        
	 */

	/* 确定所需的 relkind    */
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
			relkind = 0;		/* 让编译器安静  */
			break;
	}

	/* 锁定并验证每个关系；构建对象地址列表            */
	objects = new_object_addresses();

	foreach(cell, drop->objects)
	{
		RangeVar   *rel = makeRangeVarFromNameList((List *) lfirst(cell));
		Oid			relOid;
		ObjectAddress obj;
		struct DropRelationCallbackState state;

		/*
		 * 下面几步与 relation_openrv 非常相像，但我们不必    
		 * 费心构建 relcache 条目，因为用不到它。          
		 *
		 * 尝试访问关系之前，先检查共享缓存失效消息。  
		 * 为了覆盖这种情形：名字所标识的             
		 * 关系在事务开始后遭删除重建：若不刷新旧的  
		 * syscache 条目，我们就会沿用那个条目并在以后       
		 * 遭遇错误。                                            
		 */
		AcceptInvalidationMessages();

		/* 使用命名空间查找来找到合适的关系。      */
		state.expected_relkind = relkind;
		state.heap_lockmode = drop->concurrent ?
			ShareUpdateExclusiveLock : AccessExclusiveLock;
		/* 必须初始化这些字段，表明没有持有任何锁：    */
		state.heapOid = InvalidOid;
		state.partParentOid = InvalidOid;

		relOid = RangeVarGetRelidExtended(rel, lockmode, RVR_MISSING_OK,
										  RangeVarCallbackForDropRelation,
										  (void *) &state);

		/* 不在？  */
		if (!OidIsValid(relOid))
		{
			DropErrorMsgNonExistent(rel, relkind, drop->missing_ok);
			continue;
		}

		/*
		 * 决定这里是否使用并发模式。回调已为我们   
		 * 取回了该关系的持久性。               
		 */
		if (drop->concurrent &&
			state.actual_relpersistence != RELPERSISTENCE_TEMP)
		{
			Assert(list_length(drop->objects) == 1 &&
				   drop->removeType == OBJECT_INDEX);
			flags |= PERFORM_DELETION_CONCURRENTLY;
		}

		/*
		 * 并发索引删除也无法用于分区索引。              
		 *        
		 */
		if ((flags & PERFORM_DELETION_CONCURRENTLY) != 0 &&
			state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot drop partitioned index \"%s\" concurrently",
							rel->relname)));

		/*
		 * 如果要求删除一个分区索引，继续之前必须先获   
		 * 取其父分区表全部子对象上的锁。否则我们会       
		 * 先于其表去锁子索引分区，可能与按相反顺序锁   
		 * 定这些对象的其他会话产生潜在死锁。              
		 *                                            
		 */
		if (state.actual_relkind == RELKIND_PARTITIONED_INDEX)
			(void) find_all_inheritors(state.heapOid,
									   state.heap_lockmode,
									   NULL);

		/* 好，可以删除这一个了     */
		obj.classId = RelationRelationId;
		obj.objectId = relOid;
		obj.objectSubId = 0;

		add_exact_object_address(&obj, objects);
	}

	performMultipleDeletions(objects, drop->behavior, flags);

	free_object_addresses(objects);
}

/*
 * 在获取表锁之前，先检查我们是否拥有足够的权限。  
 * 对于 DROP INDEX，还要尝试先锁表再锁索引。此外，      
 * 如果要删除的表是一个分区，我们会先尝试锁定父表。  
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
	 * 如果我们之前已锁定其他某个索引的堆，而现在    
	 * 查找的名字不再指向那个关系，就释放无用的锁。   
	 *      
	 */
	if (relOid != oldRelOid && OidIsValid(state->heapOid))
	{
		UnlockRelationOid(state->heapOid, heap_lockmode);
		state->heapOid = InvalidOid;
	}

	/*
	 * 类似地，如果我们之前锁定了其他某个分区的堆，     
	 * 而正在查找的名字不再指向那个关系，就释放这把  
	 * 无用的锁。  
	 */
	if (relOid != oldRelOid && OidIsValid(state->partParentOid))
	{
		UnlockRelationOid(state->partParentOid, AccessExclusiveLock);
		state->partParentOid = InvalidOid;
	}

	/* 没有找到关系，因此不需要加锁或权限检查。         */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除，因此无事可做*/
	classform = (Form_pg_class) GETSTRUCT(tuple);
	is_partition = classform->relispartition;

	/* 传回部分数据，省去 RemoveRelations 查找     */
	state->actual_relkind = classform->relkind;
	state->actual_relpersistence = classform->relpersistence;

	/*
	 * RELKIND_RELATION 和 RELKIND_PARTITIONED_TABLE 都属于             
	 * OBJECT_TABLE，但 RemoveRelations() 对一个给定的关系只     
	 * 能传递一个 relkind。对普通表和分区表它都选择      
	 * RELKIND_RELATION。这意味着在关系实际为分区表           
	 * 时，必须在给出类型不符错误之前小心从事。     
	 * 索引亦然        
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

	/* 允许表的属主或模式属主执行 DROP     */
	if (!object_ownercheck(RelationRelationId, relOid, GetUserId()) &&
		!object_ownercheck(NamespaceRelationId, classform->relnamespace, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(classform->relkind),
					   rel->relname);

	/*
	 * 检查那种可能被一个失败的并发进程失效的系统       
	 * 索引的情况，并允许删除它。就目前而言，这只涉    
	 * 及在 REINDEX CONCURRENTLY 过程中变为无效的 TOAST           
	 * 关系的索引。           
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

		/* 把对象标记为系统表的无效索引               */
		if (!indisvalid)
			invalid_system_index = true;
	}

	/* 如果是无效索引，那么绕过这项检查没有问题     */
	if (!invalid_system_index && !allowSystemTableMods && IsSystemClass(relOid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rel->relname)));

	ReleaseSysCache(tuple);

	/*
	 * 在 DROP INDEX 中，尝试在锁定索引之前先获取父表  
	 * 上的锁。index_drop() 本就需要它，由于常规查询    
	 * 都是先锁表再锁其索引，如果我们反其道而行之就   
	 * 会有死锁风险。不过，如果没有找到 pg_index 条目    
	 * 也不报错 --- 该关系可能已被删除。注意这段代码    
	 * 对普通索引和分区索引都会执行。             
	 */
	if (expected_relkind == RELKIND_INDEX &&
		relOid != oldRelOid)
	{
		state->heapOid = IndexGetRelation(relOid, true);
		if (OidIsValid(state->heapOid))
			LockRelationOid(state->heapOid, heap_lockmode);
	}

	/*
	 * 类似地，如果该关系是一个分区，我们必须在锁定    
	 * 分区之前先获取其父表上的锁。这是因为查询会先   
	 * 锁定父表再锁定其分区，如果我们反其道而行就有     
	 * 风险。  
	 */
	if (is_partition && relOid != oldRelOid)
	{
		state->partParentOid = get_partition_parent(relOid, true);
		if (OidIsValid(state->partParentOid))
			LockRelationOid(state->partParentOid, AccessExclusiveLock);
	}
}

/*
 * ExecuteTruncate
 *		执行 TRUNCATE 命令。   
 *
 * 这是一次多关系截断。我们首先打开并获取所有涉  
 * 及关系上的排他锁，检查权限并核实该关系         
 * 是否可以截断。注意，如果这些关系是外部表，在       
 * 这一阶段我们还没有检查它们在外部数据源中的外   
 * 部数据是否适合截断。这些检查会在稍后实际截断     
 * 外部数据时进行。在 CASCADE 模式下，拥有指向目标       
 * 关系的 FK 引用的关系会被自动加入该组；在 RESTRICT       
 *  模式下，我们检查所有 FK 引用是否都位于正被截断     
 * 的组内部。最后，所有关系都会被截断并重建索引。   
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
	 * 打开、排他锁定并检查所有显式指定的关系。          
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

		/* 对 "TRUNCATE foo, foo" 不抛出错误   */
		if (list_member_oid(relids, myrelid))
			continue;

		/* 打开该关系，我们已经持有它的锁   */
		rel = table_open(myrelid, NoLock);

		/*
		 * RangeVarGetRelidExtended() 已通过其回调完成了大部分   
		 * 检查，但已打开 Relation 尚有其他检查。 
		 */
		truncate_check_activity(rel);

		rels = lappend(rels, rel);
		relids = lappend_oid(relids, myrelid);

		/* 仅在逻辑解码需要时记录此关系            */
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

				/* find_all_inheritors 已获得锁     */
				rel = table_open(childrelid, NoLock);

				/*
				 * 父表可能拥有其他后端的临时表作为子表， 
				 * 我们无法安全地访问这类表（由于缓冲    
				 * 问题），最好默默忽略它们。注意这一    
				 * 检查与下面调用的 truncate_check_activity()     
				 * 之一相同，为简单仍保留在这里。 
				 *                                                         
				 *                     
				 */
				if (RELATION_IS_OTHER_TEMP(rel))
				{
					table_close(rel, lockmode);
					continue;
				}

				/*
				 * 继承而来的 TRUNCATE 命令只在父表执行    
				 * 访问权限检查。故而不检查子表权限，     
				 * 不调用 truncate_check_perms()。  
				 *                             
				 */
				truncate_check_rel(RelationGetRelid(rel), rel->rd_rel);
				truncate_check_activity(rel);

				rels = lappend(rels, rel);
				relids = lappend_oid(relids, childrelid);

				/* 仅在逻辑解码需要时记录此关系            */
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

	/* 接着关闭关系 */
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * ExecuteTruncateGuts
 *
 * TRUNCATE 的内部实现。它由实际的 TRUNCATE 命令（见上文）  
 * 调用，也由执行复制 TRUNCATE 动作的订阅者调用。     
 *                            
 *
 * explicit_rels 是命令指定要截断的 Relation 列表。       
 * relids 是与 explicit_rels 对应的 Oid 列表。                   
 * relids_logged 是需要写 WAL 日志的 Oid 列表（relids       
 * 的子集）。这些信息有点冗余，但现有调用方手头      
 * 正是以这种形式持有它。   
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
	 * 检查显式指定的关系。           
	 *
	 * 在 CASCADE 模式下，把所有引用关系也吸纳进来。  
	 * 这需要多次迭代才能找到间接依赖的关系。在每一     
	 * 阶段，我们都需要在查找新关系的依赖之前先对它     
	 * 们加排他锁，否则可能漏掉一些东西。另外，我们在  
	 * 打开每个关系时就检查它，以免犯下长期锁住一        
	 * 个无权访问的关系的错误。     
	 */
	rels = list_copy(explicit_rels);
	if (behavior == DROP_CASCADE)
	{
		for (;;)
		{
			List	   *newrelids;

			newrelids = heap_truncate_find_FKs(relids);
			if (newrelids == NIL)
				break;			/* 没有其他可加  */

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

				/* 仅在逻辑解码需要时记录此关系            */
				if (RelationIsLogicallyLogged(rel))
					relids_logged = lappend_oid(relids_logged, relid);
			}
		}
	}

	/*
	 * 检查外键引用。CASCADE 模式下这一步本应         
	 * 是不必要的，因为刚拉入了全部引用；但作为    
	 * 交叉检查，在启用 Assert 时仍执行。         
	 */
#ifdef USE_ASSERT_CHECKING
	heap_truncate_check_FKs(rels, false);
#else
	if (behavior == DROP_RESTRICT)
		heap_truncate_check_FKs(rels, false);
#endif

	/*
	 * 如果被要求重启序列，就找出所有序列并锁定它们     
	 * （ResetSequence 需要 AccessExclusiveLock），并检查权限。    
	 * 我们要尽早做这件事，因为把全部截断工作做完之    
	 * 后因序列权限失败毫无意义。   
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

				/* 检查须与 AlterSequence 一致！ */
				if (!object_ownercheck(RelationRelationId, seq_relid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_SEQUENCE,
								   RelationGetRelationName(seq_rel));

				seq_relids = lappend_oid(seq_relids, seq_relid);

				relation_close(seq_rel, NoLock);
			}
		}
	}

	/* 准备捕获 AFTER 触发器。  */
	AfterTriggerBeginQuery();

	/*
	 * 为了发触发器，每个关系都需要一个 EState 和一个    
	 * ResultRelInfo。但我们无需调用 ExecOpenIndices。      
	 *
	 * 我们把 ResultRelInfo 放进 es_opened_result_relations 列表      
	 * 中，虽无范围表且不填充 es_result_relations    
	 * 数组。有点不正规，但足以让它们被                       
	 * ExecGetTriggerResultRel() 找到。 
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
						  0,	/* 伪范围表索引     */
						  NULL,
						  0);
		estate->es_opened_result_relations =
			lappend(estate->es_opened_result_relations, resultRelInfo);
		resultRelInfo++;
	}

	/*
	 * 在开始截断之前，先处理所有 BEFORE STATEMENT      
	 *  TRUNCATE 触发器（因为其中一个可能会抛出错误）。    
	 * 此外，若允许它们阻止语句执行，那将需要在         
	 * 此处处理。    
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
	 * 好，截断各表。   
	 */
	mySubid = GetCurrentSubTransactionId();

	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		/* 跳过分区表，因为没有什么要做的     */
		if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			continue;

		/*
		 * 构建各外部服务器所属外部表的列表，并传给      
		 * 外部数据包装器的回调，以便每个服务器能批量    
		 * 截断其全部外部表。每个列表作为单一条目保存 
		 * 在一张使用服务器 OID 作为查找键的哈希表中。    
		 *                          
		 */
		if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		{
			Oid			serverid = GetForeignServerIdByRelId(RelationGetRelid(rel));
			bool		found;
			ForeignTruncateInfo *ft_info;

			/* 第一次进入时，为外部表初始化哈希表         */
			if (!ft_htab)
			{
				HASHCTL		hctl;

				memset(&hctl, 0, sizeof(HASHCTL));
				hctl.keysize = sizeof(Oid);
				hctl.entrysize = sizeof(ForeignTruncateInfo);
				hctl.hcxt = CurrentMemoryContext;

				ft_htab = hash_create("TRUNCATE for Foreign Tables",
									  32,	/* 从小开始并扩展  */
									  &hctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
			}

			/* 查找或创建该外部表的缓存条目        */
			ft_info = hash_search(ft_htab, &serverid, HASH_ENTER, &found);
			if (!found)
			{
				ft_info->serverid = serverid;
				ft_info->rels = NIL;
			}

			/*
			 * 把外部表保存到其所属服务器的条目中。    
			 *                          
			 */
			ft_info->rels = lappend(ft_info->rels, rel);
			continue;
		}

		/*
		 * 通常，这里需要一次事务安全的截断。不过，若   
		 * 该表要么在当前（子）事务中创建，要么在当前    
		 * （子）事务中有新的 relfilenumber，那就可以        
		 * 直接就地截断它，因为回滚反正会使整张表或       
		 * 当前物理文件被丢弃。                              
		 */
		if (rel->rd_createSubid == mySubid ||
			rel->rd_newRelfilelocatorSubid == mySubid)
		{
			/* 立即的、不可回滚的截断是可以的*/
			heap_truncate_one_rel(rel);
		}
		else
		{
			Oid			heap_relid;
			Oid			toast_relid;
			ReindexParams reindex_params = {0};

			/*
			 * 这等效于删除表内所有行，并可在可串行化      
			 * 事务中完成。那样的话，就必须为每一个       
			 * 在该表持谓词锁的事务，向本事务记入     
			 * 一条 rw-conflict。                 
			 */
			CheckTableForSerializableConflictIn(rel);

			/*
			 * 需要完整的事务安全处理。   
			 *
			 * 为关系创建新的空存储文件，并将其指派为      
			 * relfilenumber 值。旧存储文件将在提交时删除。  
			 *                        
			 */
			RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);

			heap_relid = RelationGetRelid(rel);

			/*
			 * 对 TOAST 表（如有）亦然。   
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
			 * 重建各索引使之匹配，即告完成。    
			 */
			reindex_relation(heap_relid, REINDEX_REL_PROCESS_TOAST,
							 &reindex_params);
		}

		pgstat_count_truncate(rel);
	}

	/* 现在遍历哈希表，并截断各外部表              */
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

				/* truncate_check_rel() 已经检查过这一点 */
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
	 * 若被要求，则重启所属序列。     
	 */
	foreach(cell, seq_relids)
	{
		Oid			seq_relid = lfirst_oid(cell);

		ResetSequence(seq_relid);
	}

	/*
	 * 写一条 WAL 记录，让这组动作能够被逻辑解码。 
	 *         
	 *
	 * 组装一个 relid 数组，以便能为整个动作只写一条 WAL   
	 * 记录。    
	 */
	if (relids_logged != NIL)
	{
		xl_heap_truncate xlrec;
		int			i = 0;

		/* 只有 wal_level >= logical 才到这里     */
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
	 * 处理 AFTER STATEMENT TRUNCATE 触发器。  
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

	/* 处理排队 AFTER 触发器 */
	AfterTriggerEndQuery(estate);

	/* 现在可以清理 EState 了  */
	FreeExecutorState(estate);

	/*
	 * 关闭 CASCADE 打开的任何关系（EState 仍持              
	 * 引用）  
	 */
	rels = list_difference_ptr(rels, explicit_rels);
	foreach(cell, rels)
	{
		Relation	rel = (Relation) lfirst(cell);

		table_close(rel, NoLock);
	}
}

/*
 * 检查给定关系能否安全截断。供以下函数使用： 
 * ExecuteTruncate(),RangeVarCallbackForTruncate()。  
 */
static void
truncate_check_rel(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);

	/*
	 * 只允许对普通表、使用支持 TRUNCATE 的外部数据包   
	 * 装器的表以及分区表进行截断（不过后者包含在此     
	 * 只是为了下面的检查；对它们不会发生实际       
	 * 的物理截断。）                          
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
	 * 大多数系统表完全无法被截断，或者至少除非          
	 * allow_system_table_mods=on。但作为例外，我们允许   
	 * 在 pg_upgrade 过程中截断 pg_largeobject，因为我们需要   
	 * 修改其 relfilenode 来匹配旧集簇，而允许执行        
	 * TRUNCATE 命令是做到这一点的最简单方法。           
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
 * 检查当前用户是否具有截断给定关系的权限。          
 */
static void
truncate_check_perms(Oid relid, Form_pg_class reltuple)
{
	char	   *relname = NameStr(reltuple->relname);
	AclResult	aclresult;

	/* 权限检查       */
	aclresult = pg_class_aclcheck(relid, GetUserId(), ACL_TRUNCATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, get_relkind_objtype(reltuple->relkind),
					   relname);
}

/*
 * 一组额外的健全性检查，用于检查给定关系是否   
 * 可安全截断。同 truncate_check_rel() 分开    
 * 因 RangeVarCallbackForTruncate() 打不开 Relation。  
 */
static void
truncate_check_activity(Relation rel)
{
	/*
	 * 不允许截断其他后端的临时表……它们的本地缓冲   
	 * 管理器将无法应付。         
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot truncate temporary tables of other sessions")));

	/*
	 * 另外，也检查当前事务中该关系是否正被使用，       
	 * 含打开扫描与待处理 AFTER 触发器事件。  
	 */
	CheckTableNotInUse(rel, "TRUNCATE");
}

/*
 * storage_name
 *	  返回与 typstorage/attstorage 枚举值对应的名称            
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
 *		根据初始模式和父类返回新的模式。         
 *
 * 输入参数： 
 * 'schema' 是表的列/属性定义。（它是一个 ColumnDef         
 *		的列表。）它被破坏性地修改。   
 * 'supers' 是父关系的 OID 的列表，已由调用者锁定。        
 * 'relpersistence' 是表的持久性类型。          
 * 'is_partition' 表明该表是否为分区。    
 *
 * 输出参数：  
 * 'supconstr' 接收属于父表的约束的列表，                 
 *		并按需更新以对子表有效。           
 *
 * 返回值： 
 * 完成模式列表。 
 *
 * 注：
 *	  属性被继承的顺序非常重要。                           
 *	  直观上，被继承的属性应该排在前面。如果一张表 
 *	  继承自多个父表，那么这些属性的顺序就按照     
 *	  CREATE TABLE 中指定的父表的顺序排列。               
 *
 *	  示例如下：   
 *
 *		create table person (name text, age int4, location point);
 *		create table emp (salary int4, manager text) inherits(person);
 *		create table student (gpa float8) inherits (person);
 *		create table stud_emp (percent int4) inherits (emp, student);
 *
 *	  stud_emp 的属性顺序为：             
 *
 *							person {1:name, 2:age, 3:location}
 *							/	 \
 *			   {6:gpa}	student   emp {4:salary, 5:manager}
 *							\	 /
 *						   stud_emp {7:percent}
 *
 *	   如果同一个属性名出现多次，那么它会出现在      
 *	   结果表中它第一次出现的位置处。                       
 *
 *	   子表的约束（包括 NOT NULL 约束）是所有相关      
 *	   约束的并集，既来自子模式也来自父表。               
 *	                     
 *
 *	   子列的默认值定义如下：                  
 *		（1）如果子模式指定了默认值，就使用该值。   
 *		（2）若子模式和任何父表都未指定默认值，       
 *			该列将没有默认值。        
 *		（3）若从不同父表继承到相互冲突的默认值      
 *			（且未被子表覆盖），则会引发错误。   
 *		（4）否则使用继承来的默认值。 
 *
 *		注意，默认值基础设施亦用于生成列的表达       
 *		式，所以前面段落的大部分内容也同样适用。我们  
 *		坚持子列当且仅当其父列是生成列时才是生成    
 *		列，但它不必拥有相同的生成表达式。             
 *		                               
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
	static Node bogus_marker = {0}; /* 标记冲突的默认值   */
	List	   *saved_schema = NIL;
	ListCell   *entry;

	/*
	 * 检查并拒绝列数过多的表。我们相对较早地执行   
	 * 这项检查，原因有二：（a）避免在后续代码中       
	 * 出现 AttrNumber 溢出的风险（b）如果我们处理的列数    
	 * 不超过 1600，O(n^2) 算法没问题，但若用户试图创    
	 * 建具有数十万列的表，则可能要花上几分钟。   
	 *                      
	 *
	 * 注意我们还需要检查，在加上继承关系带来的列之后  
	 * 也没有超过这个数字。             
	 */
	if (list_length(schema) > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * 检查属性的显式列表中是否有重名。             
	 *
	 * 虽然我们可以考虑按照处理继承属性名字冲突的        
	 * 同样方式来合并这类条目，但假定这类冲突是错误   
	 * 似乎更为合理。                     
	 *
	 * 我们在这里不使用 foreach()，因为我们要对模式列表  
	 * 进行两层嵌套循环，内层循环中可能删除元素。      
	 * 如果使用 foreach_delete_current()，它只能修正其中一个   
	 * 循环的状态，所以两个循环都用基于列表索引的    
	 * 循环看起来更干净。注意任何删除都会发生在外层    
	 * 循环之后，其索引从无需调整。   
	 */
	for (int coldefpos = 0; coldefpos < list_length(schema); coldefpos++)
	{
		ColumnDef  *coldef = list_nth_node(ColumnDef, schema, coldefpos);

		if (!is_partition && coldef->typeName == NULL)
		{
			/*
			 * 类型化表列的某个选项，它不属于来自类型      
			 * 的列。这样可行，因为来自类型的列在列表   
			 * 中排在前面。（分区列清单则省略此检查，   
			 * 它们在下文中单独处理。）         
			 */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist",
							coldef->colname)));
		}

		/* restpos 扫描 coldef 之后的表项；incr 在循环体内  */
		for (int restpos = coldefpos + 1; restpos < list_length(schema);)
		{
			ColumnDef  *restdef = list_nth_node(ColumnDef, schema, restpos);

			if (strcmp(coldef->colname, restdef->colname) == 0)
			{
				if (coldef->is_from_type)
				{
					/*
					 * 把列选项合并到来自类型的列中            
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
	 * 若是分区，则没有新的列定义，只有为列约束创建的  
	 * 哑 ColumnDef。暂时先把它们放到一旁，                     
	 * 最后统一处理。   
	 */
	if (is_partition)
	{
		saved_schema = schema;
		schema = NIL;
	}

	/*
	 * 从左到右扫描各父表，并将其属性合并为一份        
	 * 继承属性清单（inhSchema）。同时检查是否                 
	 * 需要继承 OID 列。  
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

		/* 调用方已持有锁   */
		relation = table_open(parent, NoLock);

		/*
		 * 检查父分区表在当前事务内有无活跃使用，   
		 * 例如被外层命令以某种方式                        
		 * 加以使用。   
		 */
		if (is_partition)
			CheckTableNotInUse(relation, "CREATE TABLE .. PARTITION OF");

		/*
		 * 我们不允许分区表和分区参与                            
		 * 普通继承。     
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
		 * 若父表为永久表，则其全部分区也必须是永久表。 
		 * 注意继承允许该情况。    
		 */
		if (is_partition &&
			relation->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
			relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("cannot create a temporary relation as partition of permanent relation \"%s\"",
							RelationGetRelationName(relation))));

		/* 永久关系无法继承自临时关系           */
		if (relpersistence != RELPERSISTENCE_TEMP &&
			relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation \"%s\""
							: "cannot create a permanent relation as partition of temporary relation \"%s\"",
							RelationGetRelationName(relation))));

		/* 若现有关系为临时表，则必须属于本会话  */
		if (relation->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
			!relation->rd_islocaltemp)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg(!is_partition
							? "cannot inherit from temporary relation of another session"
							: "cannot create as partition of temporary relation of another session")));

		/*
		 * 本应为此提供一个 UNDER 权限标志，但目前       
		 * 只能要求子表的创建者拥有父表。       
		 */
		if (!object_ownercheck(RelationRelationId, RelationGetRelid(relation), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(relation->rd_rel->relkind),
						   RelationGetRelationName(relation));

		tupleDesc = RelationGetDescr(relation);
		constr = tupleDesc->constr;

		/*
		 * newattmap->attnums[] 将包含此父表属性对应                
		 * 的子表属性编号。（对于第一个之后的父表，    
		 * 或者在有已删除列时，编号并不相同。）           
		 */
		newattmap = make_attrmap(tupleDesc->natts);

		/* 在 newattmap 完成之前，无法处理继承的默认值。   */
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
			 * 忽略父表中已删除的列。    
			 */
			if (attribute->attisdropped)
				continue;		/* 将 newattmap->attnums 表项留为零 */

			/*
			 * 它是否与之前继承的某个列冲突？          
			 */
			exist_attno = findAttrByName(attributeName, inhSchema);
			if (exist_attno > 0)
			{
				Oid			defTypeId;
				int32		deftypmod;
				Oid			defCollId;

				/*
				 * 是的，尝试合并这两个列定义。   
				 */
				ereport(NOTICE,
						(errmsg("merging multiple inherited definitions of column \"%s\"",
								attributeName)));
				def = (ColumnDef *) list_nth(inhSchema, exist_attno - 1);

				/*
				 * 类型和 typmod 必须相同     
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
				 * 排序规则必须相同    
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
				 * 复制/检查存储参数   
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
				 * 复制/检查压缩参数       
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
				 * NOT NULL 约束的合并 = 将二者取或     
				 */
				def->is_not_null |= attribute->attnotnull;

				/*
				 * 检查 GENERATED 冲突      
				 */
				if (def->generated != attribute->attgenerated)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("inherited column \"%s\" has a generation conflict",
									attributeName)));

				/*
				 * 默认值以及其他约束将在下文处理  
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
				 * 不，创建新的继承列。   
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
			 * 若有则定位默认值/生成表达式   
			 */
			if (attribute->atthasdef)
			{
				Node	   *this_default = NULL;

				/* 在约束结构中查找默认值    */
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
				 * 若是 GENERATED 默认值，其中可能包含        
				 * 需要映射到继承列新编号的 Var。在            
				 * newattmap 就绪前无法完成，所以暂时            
				 * 记下所有继承的默认值表达式。           
				 */
				inherited_defaults = lappend(inherited_defaults, this_default);
				cols_with_defaults = lappend(cols_with_defaults, def);
			}
		}

		/*
		 * 现在处理继承的默认值表达式，借助已完成的   
		 * newattmap 来调整 attno。      
		 */
		forboth(lc1, inherited_defaults, lc2, cols_with_defaults)
		{
			Node	   *this_default = (Node *) lfirst(lc1);
			ColumnDef  *def = (ColumnDef *) lfirst(lc2);
			bool		found_whole_row;

			/* 调整 Var 使之匹配新表的列编号         */
			this_default = map_variable_attnos(this_default,
											   1, 0,
											   newattmap,
											   InvalidOid, &found_whole_row);

			/*
			 * 目前我们必须拒绝整行变量。倘若知道新表的   
			 * 行类型 OID，本可以转换，但该 OID 尚未被        
			 * 赋予。（变量仅会出现在生成表达式中，因此 
			 * 错误消息是正确的。）                          
			 */
			if (found_whole_row)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot convert whole-row table reference"),
						 errdetail("Generation expression for column \"%s\" contains a whole-row reference to table \"%s\".",
								   def->colname,
								   RelationGetRelationName(relation))));

			/*
			 * 如果先前已从某父表得到默认值，则检查两   
			 * 者是否相同。相同则没有问题；不同，则将     
			 * 该列标记为具有无效默认值。下文中如果它    
			 * 未被子模式覆盖，我们将报告错误。       
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
		 * 现在复制此父表的 CHECK 约束，借助已完成        
		 * newattmap 来调整 attno。名称相同的约束                
		 * 将尽可能合并，否则报错。        
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

				/* 若约束不可继承则忽略              */
				if (check[i].ccnoinherit)
					continue;

				/* 调整 Var 使之匹配新表的列编号         */
				expr = map_variable_attnos(stringToNode(check[i].ccbin),
										   1, 0,
										   newattmap,
										   InvalidOid, &found_whole_row);

				/*
				 * 目前我们必须拒绝整行变量。若知道新表  
				 * 的行类型 OID，本可以转换，但该 OID 尚未     
				 * 被赋予。                      
				 */
				if (found_whole_row)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot convert whole-row table reference"),
							 errdetail("Constraint \"%s\" contains a whole-row reference to table \"%s\".",
									   name,
									   RelationGetRelationName(relation))));

				/* 检查重复项     */
				if (!MergeCheckConstraint(constraints, name, expr))
				{
					/* 不，这是一个新项*/
					CookedConstraint *cooked;

					cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
					cooked->contype = CONSTR_CHECK;
					cooked->conoid = InvalidOid;	/* 直至创建  */
					cooked->name = pstrdup(name);
					cooked->attnum = 0; /* 不用于约束          */
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
		 * 关闭父关系，但在事务提交前保留其上的锁。    
		 * 这将防止其他人在子表提交之前删除或 ALTER          
		 * 父表。                     
		 */
		table_close(relation, NoLock);
	}

	/*
	 * 如果没有继承属性，结果模式只是显式声明的    
	 * 列。否则，需要将声明的列合并到继承模式             
	 * 清单中。不过，如果表是分区，则绝不会有任        
	 * 何显式声明的列。                                
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
			 * 它是否与之前继承的某个列冲突？          
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
				 * 分区只有一个父表，且没有自身的     
				 * 列定义，因此绝不应发生冲突。               
				 */
				Assert(!is_partition);

				/*
				 * 是的，尝试合并这两个列定义。   
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
				 * 类型和 typmod 必须相同     
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
				 * 排序规则必须相同    
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
				 * 标识永不继承。新列可以带有标识定义，  
				 * 因此我们总是直接采用该定义。           
				 */
				def->identity = newdef->identity;

				/*
				 * 复制存储参数    
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
				 * 复制压缩参数        
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
				 * NOT NULL 约束的合并 = 将二者取或     
				 */
				def->is_not_null |= newdef->is_not_null;

				/*
				 * 检查与生成列相关的冲突。             
				 *
				 * 如果父列是生成列，子列若还不是生成列，  
				 * 会成为生成列。若子列已是生成列，我们  
				 * 采用其生成表达式而非父列的。必须检查   
				 * 子列没有指定默认值或标识，这一点         
				 * 与 parse_utilcmd.c 中单列的                         
				 * 规则一致。                                          
				 *
				 * 反之，如果父列不是生成列，那么         
				 * 子列也无法如此。（以前曾允许，但这会    
				 * 导致可以通过父表上的 UPDATE 覆盖       
				 * 生成表达式。）                      
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
				 * 若新定义带默认值，覆盖先前默认值   
				 */
				if (newdef->raw_default != NULL)
				{
					def->raw_default = newdef->raw_default;
					def->cooked_default = newdef->cooked_default;
				}

				/* 将列标记为本地定义        */
				def->is_local = true;
			}
			else
			{
				/*
				 * 否，将新列附加到结果模式  
				 */
				inhSchema = lappend(inhSchema, newdef);
			}
		}

		schema = inhSchema;

		/*
		 * 检查在合并继承列之后，列数没有超出合法          
		 * 的上限。         
		 */
		if (list_length(schema) > MaxHeapAttributeNumber)
			ereport(ERROR,
					(errcode(ERRCODE_TOO_MANY_COLUMNS),
					 errmsg("tables can have at most %d columns",
							MaxHeapAttributeNumber)));
	}

	/*
	 * 既然我们已经有了一个分区的列定义清单，就       
	 * 可以检查列约束声明中引用的列是否确实存在。    
	 * 此外，我们把父表的 NOT NULL 约束和默认值          
	 * 合并到每个对应的列定义中。            
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
					 * 规则同上：生成与否须与父列一致，    
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
					 * 如果存在的话，则用分区的本地定义   
					 * （restdef->raw_default）覆盖此列的父       
					 * 默认值（coldef->cooked_default）。             
					 * 物理上讲，本地定义中不可能出现 cooked   
					 * 默认值，继承定义中也不会有 raw    
					 * 默认值，但请确保它们均为空，以防      
					 * 万一。       
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

			/* 对父表中不存在的列上的约束报错     */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist",
								restdef->colname)));
		}
	}

	/*
	 * 如果我们发现过相互冲突的父默认值，则检查其      
	 * 是否已被子表覆盖。       
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
 *		尝试将继承的 CHECK 约束与先前的约束合并      
 *
 * 如果我们从多个父表继承了同名约束，就必须              
 * 合并它们；若定义不完全相同，则抛出错误。           
 *
 * constraints 是先前约束的 CookedConstraint 结构体清单            
 *
 * 若已合并（约束是重复项）则返回 true；若其名      
 * 称至今唯一则返回 false；冲突则抛错。   
 */
static bool
MergeCheckConstraint(List *constraints, char *name, Node *expr)
{
	ListCell   *lc;

	foreach(lc, constraints)
	{
		CookedConstraint *ccon = (CookedConstraint *) lfirst(lc);

		Assert(ccon->contype == CONSTR_CHECK);

		/* 名称不匹配则绝不冲突    */
		if (strcmp(ccon->name, name) != 0)
			continue;

		if (equal(expr, ccon->expr))
		{
			/* 可合并   */
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
 * StoreCatalogInheritance
 *		以正确的继承信息更新系统目录。                   
 *
 * supers 是新关系直接祖先的 OID 清单。                    
 */
static void
StoreCatalogInheritance(Oid relationId, List *supers,
						bool child_is_partition)
{
	Relation	relation;
	int32		seqNumber;
	ListCell   *entry;

	/*
	 * 健全检查 
	 */
	Assert(OidIsValid(relationId));

	if (supers == NIL)
		return;

	/*
	 * 只使用直接祖先在 pg_inherits 中存储 INHERITS 信息。     
	 * 同时为直接祖先登记依赖项，并确保其                    
	 * relhassubclass 标为 true。     
	 *
	 * （很久以前，这里会找到直接和间接祖先，并         
	 * 将它们登记到 pg_ipl 中。由于该目录早已          
	 * 不存在，因此无需再查找间接祖先。）      
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
 * 创建目录项，表明 relationId 是 parentOid                    
 * 的继承子项。inhRelation 是已打开的 pg_inherits 目录。  
 */
static void
StoreCatalogInheritance1(Oid relationId, Oid parentOid,
						 int32 seqNumber, Relation inhRelation,
						 bool child_is_partition)
{
	ObjectAddress childobject,
				parentobject;

	/* 存储 pg_inherits 行    */
	StoreSingleInheritance(relationId, parentOid, seqNumber);

	/*
	 * 也存储依赖项    
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
	 * 本次继承的创建后钩子。由于 object_access_hook      
	 * 不接受多个对象标识符，我们通过 auxiliary_id      
	 * 参数转发父关系的 oid。      
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 relationId, 0,
								 parentOid, false);

	/*
	 * 将父表标记为拥有子类。    
	 */
	SetRelationHasSubclass(parentOid, true);
}

/*
 * 查找具有给定名称的现有模式表项。      
 *
 * 若属性已存在于模式中，则返回其索引（自 1 起）        
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
 * SetRelationHasSubclass
 *		在 pg_class 中设置关系的 relhassubclass 字段值。      
 *
 * 将该字段设置为 true 总是安全的，因为所有 SQL           
 * 命令都已准备好看到 true 后再发现没有子表。             
 * 若为 false 则命令假定无子表。        
 *
 * 调用方必须在事务结束前持有任何自排斥锁。              
 * 若新值为 false，调用方必须在读取支持该                    
 * 假值的证据之前就获得了该锁。这样，如果另            
 * 一个后端正在同时判定无需修改元组（新旧               
 * 值均为 true）。          
 *
 * 注意：此操作的一个重要副作用是会向外发送一            
 * 条 SI 失效消息到所有后端（包括我自己），            
 * 令引用该关系的计划用新的子表清单重建。            
 * 即使发现 pg_class 行无需更改，也必须这样。               
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
	 * 取元组的可修改副本，修改后更新 pg_class。        
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
		/* 无需修改元组，但仍强制重建 relcache           */
		CacheInvalidateRelcacheByTuple(tuple);
	}

	heap_freetuple(tuple);
	table_close(relationRelation, RowExclusiveLock);
}

/*
 * CheckRelationTableSpaceMove
 *		检查关系能否移动到新表空间。       
 *
 * 注意：调用方必须在该关系上持有                  
 *
 * AccessExclusiveLock。若关系可以移动到新表空间             
 * 则返回 true；若无法执行移动则抛错；移动无效          
 * 则返回 false。   
 */
bool
CheckRelationTableSpaceMove(Relation rel, Oid newTableSpaceId)
{
	Oid			oldTableSpaceId;

	/*
	 * 若表空间无变化则无需处理。注意 MyDatabaseTableSpace    
	 * 存为 0。 
	 */
	oldTableSpaceId = rel->rd_rel->reltablespace;
	if (newTableSpaceId == oldTableSpaceId ||
		(newTableSpaceId == MyDatabaseTableSpace && oldTableSpaceId == 0))
		return false;

	/*
	 * 我们无法支持将映射关系移动到不同表空间。         
	 * （尤其这排除了所有共享的目录。）    
	 */
	if (RelationIsMapped(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move system relation \"%s\"",
						RelationGetRelationName(rel))));

	/* 无法将非共享关系移入 pg_global         */
	if (newTableSpaceId == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only shared relations can be placed in pg_global tablespace")));

	/*
	 * 不允许移动其他后端的临时表……它们的           
	 * 缓冲区管理器无法应付。   
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move temporary tables of other sessions")));

	return true;
}

/*
 * SetRelationTableSpace
 *		在 pg_class 表项中设置新 reltablespace 与         
 *
 * relfilenumber。newTableSpaceId 是该关系的            
 * 新表空间，newRelFilenumber 是它的新文件号。    
 * 若为 InvalidRelFileNumber，则不更新。   
 *
 * 注意：调用方必须在关系上持有                     
 *
 * 本例程的调用方最好先通过调用                      
 * CheckRelationTableSpaceMove() 检查关系能否移动               
 * 到新表空间，并用下面的调用使变更可见：   
 * CommandCounterIncrement() 
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

	/* 获取该关系 pg_class 行的可修改副本。     */
	pg_class = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(reloid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", reloid);
	otid = tuple->t_self;
	rd_rel = (Form_pg_class) GETSTRUCT(tuple);

	/* 更新该 pg_class 行。*/
	rd_rel->reltablespace = (newTableSpaceId == MyDatabaseTableSpace) ?
		InvalidOid : newTableSpaceId;
	if (RelFileNumberIsValid(newRelFilenumber))
		rd_rel->relfilenode = newRelFilenumber;
	CatalogTupleUpdate(pg_class, &otid, tuple);
	UnlockTuple(pg_class, &otid, InplaceUpdateTupleLock);

	/*
	 * 记录对表空间的依赖。仅有没有物理存储的            
	 * 关系才需要这么做。   
	 */
	if (!RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		changeDependencyOnTablespace(RelationRelationId, reloid,
									 rd_rel->reltablespace);

	heap_freetuple(tuple);
	table_close(pg_class, RowExclusiveLock);
}

/*
 *		renameatt_check		- 属性重命名前的基本健全检查     
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
	 * 重命名序列或 TOAST 表的列，从系统的角度看        
	 * 并不会破坏什么，因为内部引用是通过           
	 * attnum 进行的。但允许用户修改系统里硬编码的         
	 * 名称似乎并不合适，因此有了下述                       
	 * 限制。   
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
	 * 权限检查。只有类的所有者才能更改其模式。           
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
 *		renameatt_internal		- renameatt 主力       
 *
 * 返回值是 'myrelid' 关系中的属性编号。             
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
	 * 获得目标表上的排他锁，在事务结束之前我们         
	 * 绝不释放它。       
	 */
	targetrelation = relation_open(myrelid, AccessExclusiveLock);
	renameatt_check(myrelid, RelationGetForm(targetrelation), recursing);

	/*
	 * 如果设置了 'recurse' 标志，那么还应当重命名     
	 * 所有继承自 'relname' 的类以及 'relname' 本身。         
	 *            
	 *
	 * 任何权限问题或属性重复问题都会导致整个           
	 * 事务中止，而这正是我们想要的：全有或全无。     
	 */
	if (recurse)
	{
		List	   *child_oids,
				   *child_numparents;
		ListCell   *lo,
				   *li;

		/*
		 * 我们需要每个子表的父表数目，以便对               
		 * renameatt() 的递归调用能够判断在正在处理          
		 * 的继承层次之外是否另有父表。        
		 */
		child_oids = find_all_inheritors(myrelid, AccessExclusiveLock,
										 &child_numparents);

		/*
		 * find_all_inheritors 会递归搜索继承层次，              
		 * 所以我们只需处理其返回清单中的全部 relid。       
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
		 * 如果被告知不要递归，则最好没有子表；          
		 * 否则重命名会使它们步调不一。         
		 *
		 * 只有在尚未处于递归中时，expected_parents 才        
		 */
		if (expected_parents == 0 &&
			find_inheritance_children(myrelid, NoLock) != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("inherited column \"%s\" must be renamed in child tables too",
							oldattname)));
	}

	/* 重命名复合类型的类型化表中的属性    */
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
	 * 若属性是继承而来的，则禁止重命名。如果是     
	 * 对 renameatt() 的顶层调用，那么 expected_parents             
	 * 将会是 0，因此这段代码的效果就是：只要属性       
	 * 有任何继承来源，就禁止重命名。如果是对        
	 * renameatt() 的递归调用，那么 expected_parents                  
	 * 将是当前关系在正在处理的继承层次内的                 
	 * 父表数，故仅当别处另有父表时才禁止。       
	 */
	if (attform->attinhcount > expected_parents)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot rename inherited column \"%s\"",
						oldattname)));

	/* 新名称不应当已存在       */
	(void) check_for_column_name_collision(targetrelation, newattname, false);

	/* 执行该更新  */
	namestrcpy(&(attform->attname), newattname);

	CatalogTupleUpdate(attrelation, &atttup->t_self, atttup);

	InvokeObjectPostAlterHook(RelationRelationId, myrelid, attnum);

	heap_freetuple(atttup);

	table_close(attrelation, RowExclusiveLock);

	relation_close(targetrelation, NoLock); /* 关闭 rel 但保留锁 */

	return attnum;
}

/*
 * 在获取关系锁之前执行权限和完整性检查。                 
 */
static void
RangeVarCallbackForRenameAttribute(const RangeVar *rv, Oid relid, Oid oldrelid,
								   void *arg)
{
	HeapTuple	tuple;
	Form_pg_class form;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;					/* 已被并发删除   */
	form = (Form_pg_class) GETSTRUCT(tuple);
	renameatt_check(relid, form, false);
	ReleaseSysCache(tuple);
}

/*
 *		renameatt		- 更改关系中某属性的名称             
 *
 * 返回的 ObjectAddress 是被重命名列的地址。   
 */
ObjectAddress
renameatt(RenameStmt *stmt)
{
	Oid			relid;
	AttrNumber	attnum;
	ObjectAddress address;

	/* 此处锁级别应与 renameatt_internal 一致       */
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
						   stmt->subname,	/* 旧属性名 */
						   stmt->newname,	/* 新属性名 */
						   stmt->relation->inh, /* 递归？  */
						   false,	/* 递归中  */
						   0,	/* 预期的 inhcount*/
						   stmt->behavior);

	ObjectAddressSubSet(address, RelationRelationId, relid, attnum);

	return address;
}

/*
 * 逻辑同 renameatt_internal    
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
		 * 不告知其是否在递归；这里允许更改类型化     
		 * 表。     
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
		/* 重命名索引；这同时会重命名约束         */
		RenameRelationInternal(con->conindid, newconname, false, true);
	else
		RenameConstraintById(constraintOid, newconname);

	ObjectAddressSet(address, ConstraintRelationId, constraintOid);

	ReleaseSysCache(tuple);

	if (targetrelation)
	{
		/*
		 * 使 relcache 失效，以便他人看到新约束名。          
		 */
		CacheInvalidateRelcache(targetrelation);

		relation_close(targetrelation, NoLock); /* 关闭 rel 但保留锁 */
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
		/* 此处锁级别应与 rename_constraint_internal 一致       */
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
									stmt->relation->inh),	/* 递归？  */
								   false,	/* 递归中  */
								   0 /* 预期的 inhcount*/ );
}

/*
 * 执行 ALTER TABLE/INDEX/SEQUENCE/VIEW/MATERIALIZED VIEW/FOREIGN TABLE 
 * RENAME
 */
ObjectAddress
RenameRelation(RenameStmt *stmt)
{
	bool		is_index_stmt = stmt->renameType == OBJECT_INDEX;
	Oid			relid;
	ObjectAddress address;

	/*
	 * 获得目标表、索引、序列、视图、物化视图或外   
	 * 部表上的排他锁，在事务结束之前我们绝不释        
	 * 放它。          
	 *
	 * 此处使用的锁级别应当与 RenameRelationInternal               
	 * 一致，以避免锁升级。但由于 ALTER INDEX 可以             
	 * 用于任何类型，须核实才可信。    
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
		 * 我们允许语句与对象类型不匹配（例如用           
		 * ALTER INDEX 重命名表），但可能用错了锁级别。      
		 * 若真如此，就用正确的锁级别重试。不过，        
		 * 若已对索引取得 AccessExclusiveLock 则不必。            
		 */
		relkind = get_rel_relkind(relid);
		obj_is_index = (relkind == RELKIND_INDEX ||
						relkind == RELKIND_PARTITIONED_INDEX);
		if (obj_is_index || is_index_stmt == obj_is_index)
			break;

		UnlockRelationOid(relid, lockmode);
		is_index_stmt = obj_is_index;
	}

	/* 执行工作*/
	RenameRelationInternal(relid, stmt->newname, false, is_index_stmt);

	ObjectAddressSet(address, RelationRelationId, relid);

	return address;
}

/*
 *		RenameRelationInternal - 更改关系的名称        
 */
void
RenameRelationInternal(Oid myrelid, const char *newrelname, bool is_internal, bool is_index)
{
	Relation	targetrelation;
	Relation	relrelation;	/* 用于 RELATION 关系*/
	ItemPointerData otid;
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			namespaceId;

	/*
	 * 获得目标关系上的锁，在事务结束之前我们绝           
	 * 不释放它。我们至少需要一个自排斥锁，使得   
	 * 并发的 DDL 在仍看到旧版本时就开始更新也不       
	 * 会覆盖该重命名。该锁还能防止在并发会话中     
	 * 触发 relcache 重载，因为那些会话可能无法            
	 * 正确处理这种在其眼下发生变化的信息。对于           
	 * 索引，因为 RelationReloadIndexInfo() 会特殊处              
	 * 理它。 
	 */
	targetrelation = relation_open(myrelid, is_index ? ShareUpdateExclusiveLock : AccessExclusiveLock);
	namespaceId = RelationGetNamespace(targetrelation);

	/*
	 * 查找关系的 pg_class 元组，确保 newrelname 未被使用。   
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* 不应发生     */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	otid = reltup->t_self;
	relform = (Form_pg_class) GETSTRUCT(reltup);

	if (get_relname_relid(newrelname, namespaceId) != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists",
						newrelname)));

	/*
	 * RenameRelation 特意不去相信调用方所声称的           
	 * 关系种类。这一点我们本不必操心，但也别完        
	 * 全无视它。可以把索引当作非索引来处理，        
	 * 但反过来则不允许。                
	 */
	Assert(!is_index ||
		   is_index == (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
						targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX));

	/*
	 * 用 relname 更新 pg_class 元组。（改写 reltup 无妨，     
	 * 本是副本…）     
	 */
	namestrcpy(&(relform->relname), newrelname);

	CatalogTupleUpdate(relrelation, &otid, reltup);
	UnlockTuple(relrelation, &otid, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHookArg(RelationRelationId, myrelid, 0,
								 InvalidOid, is_internal);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);

	/*
	 * 如有也重命名关联的类型。    
	 */
	if (OidIsValid(targetrelation->rd_rel->reltype))
		RenameTypeInternal(targetrelation->rd_rel->reltype,
						   newrelname, namespaceId);

	/*
	 * 如有也重命名关联的约束。          
	 */
	if (targetrelation->rd_rel->relkind == RELKIND_INDEX ||
		targetrelation->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
	{
		Oid			constraintId = get_index_constraint(myrelid);

		if (OidIsValid(constraintId))
			RenameConstraintById(constraintId, newrelname);
	}

	/*
	 * 关 rel 但留锁！     
	 */
	relation_close(targetrelation, NoLock);
}

/*
 *		ResetRelRewrite-重置 relrewrite 
 */
void
ResetRelRewrite(Oid myrelid)
{
	Relation	relrelation;	/* 用于 RELATION 关系*/
	HeapTuple	reltup;
	Form_pg_class relform;

	/*
	 * 取该 pg_class 元组。      
	 */
	relrelation = table_open(RelationRelationId, RowExclusiveLock);

	reltup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(myrelid));
	if (!HeapTupleIsValid(reltup))	/* 不应发生     */
		elog(ERROR, "cache lookup failed for relation %u", myrelid);
	relform = (Form_pg_class) GETSTRUCT(reltup);

	/*
	 * 改 pg_class 行。   
	 */
	relform->relrewrite = InvalidOid;

	CatalogTupleUpdate(relrelation, &reltup->t_self, reltup);

	heap_freetuple(reltup);
	table_close(relrelation, RowExclusiveLock);
}

/*
 * 禁止在后端对目标表持有除当前命令刚获得的            
 * 引用之外的打开引用时执行 ALTER TABLE（及类似           
 * 命令）；这意味着存在打开的游标或活跃的计划。      
 * 需要这个检查，因为我们的锁防不住自己踩自己        
 * 的脚，只能防别人的脚！         
 *
 * 对 ALTER TABLE 来说，已知会引发严重问题的只            
 * 有 ALTER COLUMN TYPE；有些变更显然相当无害，               
 * 所以本可以放宽为只对某些类型的修改报错。           
 * 但允许这些操作的场景并不明显，所以暂时              
 * 不为之费心。            
 *
 * 如果该关系存在待处理的 AFTER 触发器事件，我              
 * 们也会拒绝这些命令。对于带重写的 ALTER TABLE           
 * 变体这肯定是必需的，因为它们不保留元组             
 * TID，待处理的事件将尝试取到错误的元组。               
 * 在其他情况下这可能过于谨慎，但宁可多疑也             
 *
 * REINDEX 调用它时 "rel" 指向要重建的索引；这里           
 * 我们担心的是该索引上活跃的索引扫描。触发           
 * 器事件检查可以跳过，因为我们不会损坏父              
 *
 * 表。语句名（如 "ALTER TABLE"）会传入错误消息。             
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
 * CheckAlterTableIsSafe
 *		验证在此关系上允许 ALTER TABLE 是安全的。     
 *
 * 这由 CheckTableNotInUse() 外加一项检查组成：即          
 * 确认该关系不是其他会话的临时表。这项临            
 * 时表检查必须拆出来，因为有些调用方不需要           
 * 这项检查，最显著的是 DROP TABLE。（必须允许            
 * DROP，否则无法清理孤儿临时模式。）            
 */
static void
CheckAlterTableIsSafe(Relation rel)
{
	/*
	 * 不允许对其他后端的临时表执行 ALTER。如果我们        
	 * 需要改变表内容，它们的本地缓冲区管理器将           
	 * 无法应付。即便不需要，也可能存在假定临时        
	 * 表不受这类干扰的优化。   
	 */
	if (RELATION_IS_OTHER_TEMP(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter temporary tables of other sessions")));

	/*
	 * 同时检查该关系在当前事务内的活跃使用，包括       
	 * 打开的扫描与待处理 AFTER 触发器事件。  
	 */
	CheckTableNotInUse(rel, "ALTER TABLE");
}

/*
 * AlterTableLookupRelation
 *		查找并锁定 ALTER TABLE 语句所命名关系的 OID。        
 *		          
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
 *		执行 ALTER TABLE，可以是子命令清单。       
 *
 * ALTER TABLE 分三个阶段执行：     
 *		1. 审查子命令并执行前置转换检查。               
 *		2. 校验并转换子命令，更新系统目录。               
 *		3. 扫描表以检查新约束，并可选择地把数据       
 *		   再复制到新表中。   
 * 只有当一个或多个子命令需要时才执行阶段 3。         
 * 这样设计的意图是允许对表模式做多处               
 * 相互独立的更新，而对数据只扫一遍。                    
 *      
 *
 * ATPrepCmd 执行阶段 1。为每张受影响的表创建       
 * 一个"工作队列"项（若命令遍历表继承层次，            
 * 则可能有多张受影响的表）。我们还会做初步             
 * 的子命令校验。由于较早的子命令可能改变后          
 * 继命令看到的目录状态，这一阶段能做的事情          
 * 是有限的。通常这一阶段会获取表锁、检查权       
 * 限和 relkind，并递归查找子表。                         
 *
 * ATRewriteCatalogs 执行每张受影响表的阶段 2。   
 * 某些子命令必须先于其他命令执行以避免不必   
 * 要的冲突；例如 DROP COLUMN 应当先于 ADD COLUMN。      
 * 因此阶段 1 把子命令划分成多个清单，对应阶段     
 * 2 的每个逻辑"遍次"。                  
 *
 * ATRewriteTables 为需要它的表执行阶段 3。            
 *
 * 对大多数子命令类型而言，阶段 2 和 3 不做显式    
 * 递归，因为阶段 1 已经做了。不过，某些子命令      
 * 类型只有在阶段 2 才能确定递归方式；这类情        
 * 形下由阶段 1 设置 cmd->recurse 标志。 
 *
 * 多亏 MVCC 的魔力，过程中任何位置的错误都会           
 * 回滚整个操作；我们无需做任何专门的清理。          
 *
 * 调用方必须锁定该关系，锁级别应适合所请求     
 * 的子命令，使用 AlterTableGetLockLevel(stmt->cmds)               
 * 或更高。我们将锁级别        
 * 向下传递，以便递归地应用到继承表。注意，          
 * 我们递归时想要的锁级别很可能高于该具体子           
 * 命令所需的级别。所以我们向下传递整体锁需           
 * 求，而非在更低层重估。       
 *
 * 调用方还提供一个"context"，当我们需要执行         
 * 诸如 CREATE INDEX 之类的子命令时会传回                  
 * utility.c。其中某些字段（如 relid）这里也会用到      
 */
void
AlterTable(AlterTableStmt *stmt, LOCKMODE lockmode,
		   AlterTableUtilityContext *context)
{
	Relation	rel;

	/* 需要调用方提供足够级别的锁。      */
	rel = relation_open(context->relid, NoLock);

	CheckAlterTableIsSafe(rel);

	ATController(stmt, rel, stmt->cmds, stmt->relation->inh, lockmode, context);
}

/*
 * AlterTableInternal
 *
 * OID 指定目标的 ALTER TABLE。      
 *
 * 如果该关系已经被打开，我们并不拒绝，              
 * 因为很可能某一层或几层调用方都打开着它。         
 * 这意味着对可能破坏现有查询计划的修改而言，   
 * 使用此入口是不安全的。假定它不用于此类目       
 * 的，待处理 AFTER 触发器亦无需拒绝。   
 *
 * 此外，由于没有 AlterTableUtilityContext，它无法           
 * 用于任何需要解析转换的子命令类型，或者         
 * 生成必须传给 ProcessUtility 的子命令的类型。          
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
 * AlterTableGetLockLevel
 *
 * 为给定的子命令清单设置所需的整体锁级别。              
 * 具体策略按 AlterTable() 的需要制定，整体解释      
 * 参见那里的注释。               
 *
 * 该函数在解析前后都会调用，因此每次调用，     
 * 都必须给出相同的答案。有些子命令会被转换   
 * 成其他子命令类型，因此转换决不允许落到比         
 * 先前分配的更低的锁级别。所有转换都在下文注           
 *
 * 由于本函数在锁定表之前调用，我们无法用表              
 * 元数据影响要加的锁类型。     
 *
 * 子命令函数中不应硬编码任何锁模式。ALTER TABLE           
 * 的锁模式决策只在这里做出。唯一的例外是                  
 * ALTER TABLE RENAME，它被视为另一种语句类型                        
 * T_RenameStmt，不经过这段代码，也无法与此处给                
 * 出的任何子命令组合。    
 *
 * 注意热备只知道主库上的 AccessExclusiveLock                     
 * 因此任何可能影响备库上 SELECT 运行的变更都             
 * 需要使用 AccessExclusiveLock，即使你认为更低的              
 * 锁即可，除非有解。   
 *
 * 另外注意 pg_dump 只使用 AccessShareLock，这意味着             
 * 任何小于 AccessExclusiveLock 的锁都可以在 pg_dump                  
 * 运行期间更改对象定义。请小心检查适当的数据           
 * 是 pg_dump 用 MVCC 快照取的，而不是通过 syscache            
 * 查找，否则最终可能得到无法恢复的不一致转储。     
 */
LOCKMODE
AlterTableGetLockLevel(List *cmds)
{
	/*
	 * 仅当用 MVCC 快照读目录表时这才有效。            
	 */
	ListCell   *lcmd;
	LOCKMODE	lockmode = ShareUpdateExclusiveLock;

	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		LOCKMODE	cmd_lockmode = AccessExclusiveLock; /* 编译器默认值   */

		switch (cmd->subtype)
		{
				/*
				 * 这些子命令会重写堆，因此需要完全锁。    
				 */
			case AT_AddColumn:	/* may rewrite heap, in some cases and visible
								 * to SELECT */
			case AT_SetAccessMethod:	/* 必须重写堆   */
			case AT_SetTableSpace:	/* 必须重写堆   */
			case AT_AlterColumnType:	/* 必须重写堆   */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 这些子命令可能需要添加 TOAST 表。如果      
				 * 我们给正在被扫描的表添加 TOAST 表，可能会 
				 * 错过并发插入事务加入新 TOAST 表的数据。   
				 *                     
				 */
			case AT_SetStorage: /* may add toast tables, see
								 * ATRewriteCatalogs() */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 删除约束可能影响那些假设约束成立的   
				 * 已优化的 SELECT 查询。另见                   
				 * CloneFkReferenced 
				 */
			case AT_DropConstraint: /* 同 DROP INDEX*/
			case AT_DropNotNull:	/* 或改变某些 SQL 计划*/
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 可能对并发 SELECT 可见的子命令            
				 */
			case AT_DropColumn: /* SELECT 可见的变更   */
			case AT_AddColumnToView:	/* 创建视图*/
			case AT_DropOids:	/* 曾经等同于 DropColumn  */
			case AT_EnableAlwaysRule:	/* 或改变 SELECT 规则 */
			case AT_EnableReplicaRule:	/* 或改变 SELECT 规则 */
			case AT_EnableRule: /* 或改变 SELECT 规则 */
			case AT_DisableRule:	/* 或改变 SELECT 规则 */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 更改所有者可能移除隐式的 SELECT 权限  
				 */
			case AT_ChangeOwner:	/* SELECT 可见的变更   */
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 更改外部表选项可能影响优化。             
				 */
			case AT_GenericOptions:
			case AT_AlterColumnGenericOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 这些子命令只影响写操作。           
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
				 * 这些子命令只影响写操作。XXX            
				 * 理论上可以是 ShareRowExclusiveLock。         
				 */
			case AT_ColumnDefault:
			case AT_CookedColumnDefault:
			case AT_AlterConstraint:
			case AT_AddIndex:	/* 由 ADD CONSTRAINT  */
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
			case AT_ReAddConstraint:	/* 变成 AT_AddConstraint  */
			case AT_ReAddDomainConstraint:	/* 变成 AT_AddConstraint  */
				if (IsA(cmd->def, Constraint))
				{
					Constraint *con = (Constraint *) cmd->def;

					switch (con->contype)
					{
						case CONSTR_EXCLUSION:
						case CONSTR_PRIMARY:
						case CONSTR_UNIQUE:

							/*
							 * 情形同 CREATE INDEX。若能解决并      
							 * 发目录更新的问题，锁强度就能降 
							 * 到 ShareLock。XXX 或许可以下调到      
							 * ShareRowExclusiveLock，还需   
							 * 进一步分析。                        
							 *          
							 */
							cmd_lockmode = AccessExclusiveLock;
							break;
						case CONSTR_FOREIGN:

							/*
							 * 加外键时两个表都会加触发器，  
							 * 因此锁级别至少必须与 CREATE TRIGGER  
							 * 相当。                   
							 */
							cmd_lockmode = ShareRowExclusiveLock;
							break;

						default:
							cmd_lockmode = AccessExclusiveLock;
					}
				}
				break;

				/*
				 * 这些子命令影响继承行为。在我们之前    
				 * 启动的查询将继续看到旧继承行为，而在    
				 * 提交后启动的查询将看到新行为。不过      
				 * 在挂接子表时无需阻止其读写。但改变     
				 * TupDesc 可能有问题，因此保留最高锁。       
				 *                                        
				 */
			case AT_AddInherit:
			case AT_DropInherit:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 这些子命令影响隐式行类型转换。其对查     
				 * 询的影响同 CREATE/DROP CAST 类似。不提供       
				 * 因这类变更而使解析树失效的机制，所以  
				 * 们把它们保持在 AccessExclusiveLock。     
				 */
			case AT_AddOf:
			case AT_DropOf:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 仅 CREATE OR REPLACE VIEW 使用，它必须与       
				 * 正在使用它的 SELECT 冲突。      
				 */
			case AT_ReplaceRelOptions:
				cmd_lockmode = AccessExclusiveLock;
				break;

				/*
				 * 这些子命令影响性能和维护策略，但           
				 * 不改变正常数据读写的语义结果。把         
				 * ALTER TABLE 挪到当前活跃写操作之后，          
				 * 只会推迟新策略生效的时点，等待没          
				 * 有好处。这种情况下适用最低限制         
				 * ：目前不允许                                      
				 * 并发的目录更新。                            
				 *         
				 */
			case AT_SetStatistics:	/* getTableAttrs() 用 MVCC     */
			case AT_ClusterOn:	/* getIndexes() 用 MVCC     */
			case AT_DropCluster:	/* getIndexes() 用 MVCC     */
			case AT_SetOptions: /* getTableAttrs() 用 MVCC     */
			case AT_ResetOptions:	/* getTableAttrs() 用 MVCC     */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

			case AT_SetLogged:
			case AT_SetUnLogged:
				cmd_lockmode = AccessExclusiveLock;
				break;

			case AT_ValidateConstraint: /* getConstraints() 用 MVCC     */
				cmd_lockmode = ShareUpdateExclusiveLock;
				break;

				/*
				 * 关系选项比乍看更复杂。这里的选项为     
				 * 表、视图和索引设置；因历史原因，它们    
				 * 都能与 ALTER TABLE 一起使用，因此无法用基    
				 * 本语法来区分它们。                 
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
				 * 这里只检查表的模式；但锁必须足够强    
				 * ，以阻止并发的 DROP NOT NULL。            
				 */
				cmd_lockmode = AccessShareLock;
				break;

			default:			/* 唉  */
				elog(ERROR, "unrecognized alter table type: %d",
					 (int) cmd->subtype);
				break;
		}

		/*
		 * 取各子命令中最大的锁模式          
		 */
		if (cmd_lockmode > lockmode)
			lockmode = cmd_lockmode;
	}

	return lockmode;
}

/*
 * ATController 为各阶段提供顶层控制。          
 *
 * 传入 parsetree，以便在被请求时将其传给事件       
 * 触发器。   
 */
static void
ATController(AlterTableStmt *parsetree,
			 Relation rel, List *cmds, bool recurse, LOCKMODE lockmode,
			 AlterTableUtilityContext *context)
{
	List	   *wqueue = NIL;
	ListCell   *lcmd;

	/* 阶段 1：对各命令做初步检查，并创建工作队列  */
	foreach(lcmd, cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

		ATPrepCmd(&wqueue, rel, cmd, recurse, false, lockmode, context);
	}

	/* 关闭关系，但保留锁直至事务提交  */
	relation_close(rel, NoLock);

	/* 阶段 2：更新系统目录   */
	ATRewriteCatalogs(&wqueue, lockmode, context);

	/* 阶段 3：按需扫描/重写表，并运行 afterStmts   */
	ATRewriteTables(parsetree, &wqueue, lockmode, context);
}

/*
 * ATPrepCmd
 *
 * ALTER TABLE 阶段 1 各操作的交通警察，包括简单    
 * 递归和权限检查。        
 *
 * 调用者必须已在该关系上持有适当类型的锁。        
 * 该锁应持有到提交为止。     
 */
static void
ATPrepCmd(List **wqueue, Relation rel, AlterTableCmd *cmd,
		  bool recurse, bool recursing, LOCKMODE lockmode,
		  AlterTableUtilityContext *context)
{
	AlteredTableInfo *tab;
	int			pass = AT_PASS_UNSET;

	/* 为该表查找或创建工作队列条目     */
	tab = ATGetQueueEntry(wqueue, rel);

	/*
	 * 对处于待分离状态的分区，禁止除 DETACH FINALIZE     
	 * 之外的任何 ALTER TABLE。     
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
	 * 为每个表复制原始子命令，以便能在其上任意涂改。 
	 * 这样可避免冲突：当不同子表需要做              
	 * 不同的语法分析变换（例如同一列在不同                
	 * 子表中可能有不同的列号）。         
	 */
	cmd = copyObject(cmd);

	/*
	 * 做权限和 relkind 检查，如有需要递归到子表，     
	 * 以及任何所需的额外阶段 1 处理。（但要当心添加   
	 * 那些会查看表细节的处理，这些细节可能被另  
	 * 一个子命令改变。某些情况下我们会拒绝多             
	 * 个以相反方式改变同一状态的子命令。）    
	 */
	switch (cmd->subtype)
	{
		case AT_AddColumn:		/* 添加列  */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, false, cmd,
							lockmode, context);
			/* 递归发生在执行阶段             */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_AddColumnToView:	/* 经 CREATE OR REPLACE VIEW 添加列  */
			ATSimplePermissions(cmd->subtype, rel, ATT_VIEW);
			ATPrepAddColumn(wqueue, rel, recurse, recursing, true, cmd,
							lockmode, context);
			/* 递归发生在执行阶段             */
			pass = AT_PASS_ADD_COL;
			break;
		case AT_ColumnDefault:	/* 更改列默认值   */

			/*
			 * 允许视图带默认值，这样向视图执行 INSERT 时   
			 * 会有类似默认值的行为。因为重写器会   
			 * 先把默认值代入 INSERT，之后再展开规则。  
			 *       
			 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备     */
			pass = cmd->def ? AT_PASS_ADD_OTHERCONSTR : AT_PASS_DROP;
			break;
		case AT_CookedColumnDefault:	/* 添加预置的默认值 */
			/* 目前仅在 CREATE TABLE 中使用         */
			/* （因此权限检查其实并无必要）       */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_AddIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			pass = AT_PASS_ADD_OTHERCONSTR;
			break;
		case AT_SetIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			/* 需在 AddIdentity 之后运行，故置于 MISC 遍     */
			pass = AT_PASS_MISC;
			break;
		case AT_DropIdentity:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			pass = AT_PASS_DROP;
			break;
		case AT_DropNotNull:	/* 移除列上的 NOT NULL   */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATPrepDropNotNull(rel, recurse, recursing);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			pass = AT_PASS_DROP;
			break;
		case AT_SetNotNull:		/* 设置列上的 NOT NULL  */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 需要针对特定命令的递归决策  */
			ATPrepSetNotNull(wqueue, rel, cmd, recurse, recursing,
							 lockmode, context);
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_CheckNotNull:	/* 检查列是否已标记为 NOT NULL    */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备     */
			pass = AT_PASS_COL_ATTRS;
			break;
		case AT_DropExpression: /* 移除列的生成表达式  */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			ATPrepDropExpression(rel, cmd, recurse, recursing, lockmode);
			pass = AT_PASS_DROP;
			break;
		case AT_SetStatistics:	/* 设置列统计信息目标 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX | ATT_PARTITIONED_INDEX | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_SetOptions:		/* 设置列的 ( 选项 )      */
		case AT_ResetOptions:	/* 重置列的 ( 选项 )        */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			pass = AT_PASS_MISC;
			break;
		case AT_SetStorage:		/* 设置列的存储策略 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_FOREIGN_TABLE);
			ATSimpleRecursion(wqueue, rel, cmd, recurse, lockmode, context);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_SetCompression: /* 设置列的压缩方法     */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_DropColumn:		/* 删除列   */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			ATPrepDropColumn(wqueue, rel, recurse, recursing, cmd,
							 lockmode, context);
			/* 递归发生在执行阶段             */
			pass = AT_PASS_DROP;
			break;
		case AT_AddIndex:		/* 加索引 */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_ADD_INDEX;
			break;
		case AT_AddConstraint:	/* 添加约束   */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 递归发生在执行阶段             */
			/* 除了保存递归标志外，无需特定命令的准备  */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_ADD_CONSTR;
			break;
		case AT_AddIndexConstraint: /* 添加基于索引的约束*/
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_ADD_INDEXCONSTR;
			break;
		case AT_DropConstraint: /* 删除约束    */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			ATCheckPartitionsNotInUse(rel, lockmode);
			/* 其余递归发生在执行阶段             */
			/* 除了保存递归标志外，无需特定命令的准备  */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_DROP;
			break;
		case AT_AlterColumnType:	/* 更改列类型   */
			ATSimplePermissions(cmd->subtype, rel,
								ATT_TABLE | ATT_COMPOSITE_TYPE | ATT_FOREIGN_TABLE);
			/* 参见 ATPrepAlterColumnType 的注释 */
			cmd = ATParseTransformCmd(wqueue, tab, rel, cmd, recurse, lockmode,
									  AT_PASS_UNSET, context);
			Assert(cmd != NULL);
			/* 自行执行递归     */
			ATPrepAlterColumnType(wqueue, tab, rel, recurse, recursing, cmd,
								  lockmode, context);
			pass = AT_PASS_ALTER_TYPE;
			break;
		case AT_AlterColumnGenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_ChangeOwner:	/* ALTER OWNER */
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_ClusterOn:		/* CLUSTER ON */
		case AT_DropCluster:	/* 设置无聚簇     */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			/* 这些命令不会递归     */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_SetLogged:		/* SET LOGGED */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_SEQUENCE);
			if (tab->chgPersistence)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot change persistence setting twice")));
			tab->chgPersistence = ATPrepChangePersistence(rel, true);
			/* 必要时强制重写；参见 ATRewriteTables 中的注释*/
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
			/* 必要时强制重写；参见 ATRewriteTables 中的注释*/
			if (tab->chgPersistence)
			{
				tab->rewrite |= AT_REWRITE_ALTER_PERSISTENCE;
				tab->newrelpersistence = RELPERSISTENCE_UNLOGGED;
			}
			pass = AT_PASS_MISC;
			break;
		case AT_DropOids:		/* 设置无 OIDS   */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			pass = AT_PASS_DROP;
			break;
		case AT_SetAccessMethod:	/* 设置访问方法*/
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);

			/* 分区表没有访问方法                    */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("cannot change access method of a partitioned table")));

			/* 检查是否已请求过其他访问方法更改            */
			if (OidIsValid(tab->newAccessMethod))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot have multiple SET ACCESS METHOD subcommands")));

			ATPrepSetAccessMethod(tab, rel, cmd->name);
			pass = AT_PASS_MISC;	/* 无关紧要；Phase 2 无需操作 */
			break;
		case AT_SetTableSpace:	/* 设置表空间*/
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW | ATT_INDEX |
								ATT_PARTITIONED_INDEX);
			/* 此命令不会递归       */
			ATPrepSetTableSpace(tab, rel, cmd->name, lockmode);
			pass = AT_PASS_MISC;	/* 其实并无影响      */
			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* 重置 (...)*/
		case AT_ReplaceRelOptions:	/* 全部重置后，仅设置这些   */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_VIEW | ATT_MATVIEW | ATT_INDEX);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_AddInherit:		/* 继承  */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			ATPrepAddInherit(rel);
			pass = AT_PASS_MISC;
			break;
		case AT_DropInherit:	/* 无继承  */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_AlterConstraint:	/* 更改约束     */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 递归发生在执行阶段             */
			pass = AT_PASS_MISC;
			break;
		case AT_ValidateConstraint: /* 验证约束        */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 递归发生在执行阶段             */
			/* 除了保存递归标志外，无需特定命令的准备  */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_ReplicaIdentity:	/* 复制标识 ...     */
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_MATVIEW);
			pass = AT_PASS_MISC;
			/* 此命令不会递归       */
			/* 无需特定命令的准备     */
			break;
		case AT_EnableTrig:		/* 启用触发器变体   */
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:	/* 禁用触发器变体    */
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);
			/* 为 Phase 2 设置递归；无需其他准备      */
			if (recurse)
				cmd->recurse = true;
			pass = AT_PASS_MISC;
			break;
		case AT_EnableRule:		/* 启用/禁用规则变体    */
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
			/* 这些命令不会递归     */
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_GenericOptions:
			ATSimplePermissions(cmd->subtype, rel, ATT_FOREIGN_TABLE);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_AttachPartition:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE | ATT_PARTITIONED_INDEX);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartition:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		case AT_DetachPartitionFinalize:
			ATSimplePermissions(cmd->subtype, rel, ATT_TABLE);
			/* 无需特定命令的准备     */
			pass = AT_PASS_MISC;
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			pass = AT_PASS_UNSET;	/* 安抚编译器     */
			break;
	}
	Assert(pass > AT_PASS_UNSET);

	/* 将该子命令加入 Phase 2 的对应列表          */
	tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd);
}

/*
 * ATRewriteCatalogs
 *
 * ALTER TABLE Phase 2 操作的总调度台。各子命令按     
 * "安全"的执行顺序分派（其设计旨在避免不必要的冲 
 * 突）。  
 */
static void
ATRewriteCatalogs(List **wqueue, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	int			pass;
	ListCell   *ltab;

	/*
	 * 我们"并行"处理所有表，一次一个遍。这是因为       
	 * 工作可能需要从一个表传播到另一个表（具体而言， 
	 * 对外键的 PK 执行 ALTER TYPE 时必须向另一个表分派   
	 * 重新添加外键约束的工作）。不过，工作只能传播    
	 * 到更靠后的遍。                         
	 */
	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* 遍历每个需要处理的表                   */
		foreach(ltab, *wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			/*
			 * 打开关系并存储在 tab 中，从而允许子例程在需 
			 * 要时关闭并重新打开。合适的锁已由 Phase 1 获  
			 * 得，无需再次获取。      
			 */
			tab->rel = relation_open(tab->relid, NoLock);

			foreach(lcmd, subcmds)
				ATExecCmd(wqueue, tab,
						  lfirst_node(AlterTableCmd, lcmd),
						  lockmode, pass, context);

			/*
			 * ALTER TYPE 遍之后做清理工作（该工作不在          
			 * ATExecAlterColumnType 内做，因为表的多列同时   
			 * 被更改时只应执行一次）。     
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

	/* 检查是否必须添加 TOAST 表。        */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/*
		 * 如果该表是 ATTACH PARTITION 命令的源表，我们       
		 * 没有改动影响 TOAST 需求的内容，无需检查   
		 * 。                              
		 */
		if (((tab->relkind == RELKIND_RELATION ||
			  tab->relkind == RELKIND_PARTITIONED_TABLE) &&
			 tab->partition_constraint == NULL) ||
			tab->relkind == RELKIND_MATVIEW)
			AlterTableCreateToastTable(tab->relid, (Datum) 0, lockmode);
	}
}

/*
 * ATExecCmd：将子命令分派给相应的执行例程           
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
		case AT_AddColumn:		/* 添加列  */
		case AT_AddColumnToView:	/* 经 CREATE OR REPLACE VIEW 添加列  */
			address = ATExecAddColumn(wqueue, tab, rel, &cmd,
									  cmd->recurse, false,
									  lockmode, cur_pass, context);
			break;
		case AT_ColumnDefault:	/* 更改列默认值   */
			address = ATExecColumnDefault(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_CookedColumnDefault:	/* 添加预置的默认值 */
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
		case AT_DropNotNull:	/* 移除列上的 NOT NULL   */
			address = ATExecDropNotNull(rel, cmd->name, lockmode);
			break;
		case AT_SetNotNull:		/* 设置列上的 NOT NULL  */
			address = ATExecSetNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_CheckNotNull:	/* 检查列是否已标记为 NOT NULL    */
			ATExecCheckNotNull(tab, rel, cmd->name, lockmode);
			break;
		case AT_DropExpression:
			address = ATExecDropExpression(rel, cmd->name, cmd->missing_ok, lockmode);
			break;
		case AT_SetStatistics:	/* 设置列统计信息目标 */
			address = ATExecSetStatistics(rel, cmd->name, cmd->num, cmd->def, lockmode);
			break;
		case AT_SetOptions:		/* 设置列的 ( 选项 )      */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, false, lockmode);
			break;
		case AT_ResetOptions:	/* 重置列的 ( 选项 )        */
			address = ATExecSetOptions(rel, cmd->name, cmd->def, true, lockmode);
			break;
		case AT_SetStorage:		/* 设置列的存储策略 */
			address = ATExecSetStorage(rel, cmd->name, cmd->def, lockmode);
			break;
		case AT_SetCompression: /* 设置列的压缩方法     */
			address = ATExecSetCompression(rel, cmd->name, cmd->def,
										   lockmode);
			break;
		case AT_DropColumn:		/* 删除列   */
			address = ATExecDropColumn(wqueue, rel, cmd->name,
									   cmd->behavior, cmd->recurse, false,
									   cmd->missing_ok, lockmode,
									   NULL);
			break;
		case AT_AddIndex:		/* 加索引 */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, false,
									 lockmode);
			break;
		case AT_ReAddIndex:		/* 加索引 */
			address = ATExecAddIndex(tab, rel, (IndexStmt *) cmd->def, true,
									 lockmode);
			break;
		case AT_ReAddStatistics:	/* 添加统计   */
			address = ATExecAddStatistics(tab, rel, (CreateStatsStmt *) cmd->def,
										  true, lockmode);
			break;
		case AT_AddConstraint:	/* 添加约束   */
			/* 仅在初始检查阶段对该命令进行转换      */
			if (cur_pass == AT_PASS_ADD_CONSTR)
				cmd = ATParseTransformCmd(wqueue, tab, rel, cmd,
										  cmd->recurse, lockmode,
										  cur_pass, context);
			/* 取决于约束类型，现在可能无需再做其他工作  */
			if (cmd != NULL)
				address =
					ATExecAddConstraint(wqueue, tab, rel,
										(Constraint *) cmd->def,
										cmd->recurse, false, lockmode);
			break;
		case AT_ReAddConstraint:	/* 重新添加已有的 CHECK 约束   */
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
		case AT_ReAddComment:	/* 重新添加已有注释*/
			address = CommentObject((CommentStmt *) cmd->def);
			break;
		case AT_AddIndexConstraint: /* 添加基于索引的约束*/
			address = ATExecAddIndexConstraint(tab, rel, (IndexStmt *) cmd->def,
											   lockmode);
			break;
		case AT_AlterConstraint:	/* 更改约束     */
			address = ATExecAlterConstraint(rel, cmd, false, false, lockmode);
			break;
		case AT_ValidateConstraint: /* 验证约束        */
			address = ATExecValidateConstraint(wqueue, rel, cmd->name, cmd->recurse,
											   false, lockmode);
			break;
		case AT_DropConstraint: /* 删除约束    */
			ATExecDropConstraint(rel, cmd->name, cmd->behavior,
								 cmd->recurse, false,
								 cmd->missing_ok, lockmode);
			break;
		case AT_AlterColumnType:	/* 更改列类型   */
			/* 解析转换已在前面完成        */
			address = ATExecAlterColumnType(tab, rel, cmd, lockmode);
			break;
		case AT_AlterColumnGenericOptions:	/* 更改列选项      */
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
		case AT_DropCluster:	/* 设置无聚簇     */
			ATExecDropCluster(rel, lockmode);
			break;
		case AT_SetLogged:		/* SET LOGGED */
		case AT_SetUnLogged:	/* SET UNLOGGED */
			break;
		case AT_DropOids:		/* 设置无 OIDS   */
			/* 此处无需处理，OID 列已不复存在         */
			break;
		case AT_SetAccessMethod:	/* 设置访问方法*/
			/* 在 Phase 3 中专门处理  */
			break;
		case AT_SetTableSpace:	/* 设置表空间*/

			/*
			 * 只处理分区表和分区索引：对它们而言这只是   
			 * 目录更改。其他具有存储的关系类型则由    
			 * Phase 3 处理。              
			 */
			if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ||
				rel->rd_rel->relkind == RELKIND_PARTITIONED_INDEX)
				ATExecSetTableSpaceNoStorage(rel, tab->newTableSpace);

			break;
		case AT_SetRelOptions:	/* SET (...) */
		case AT_ResetRelOptions:	/* 重置 (...)*/
		case AT_ReplaceRelOptions:	/* 替换整个选项列表   */
			ATExecSetRelOptions(rel, (List *) cmd->def, cmd->subtype, lockmode);
			break;
		case AT_EnableTrig:		/* ENABLE TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_ORIGIN, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableAlwaysTrig:	/* ENABLE ALWAYS TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ALWAYS, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_EnableReplicaTrig:	/* ENABLE REPLICA TRIGGER name */
			ATExecEnableDisableTrigger(rel, cmd->name,
									   TRIGGER_FIRES_ON_REPLICA, false,
									   cmd->recurse,
									   lockmode);
			break;
		case AT_DisableTrig:	/* DISABLE TRIGGER name */
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

		case AT_EnableRule:		/* ENABLE RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_ORIGIN, lockmode);
			break;
		case AT_EnableAlwaysRule:	/* ENABLE ALWAYS RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ALWAYS, lockmode);
			break;
		case AT_EnableReplicaRule:	/* ENABLE REPLICA RULE name */
			ATExecEnableDisableRule(rel, cmd->name,
									RULE_FIRES_ON_REPLICA, lockmode);
			break;
		case AT_DisableRule:	/* DISABLE RULE name */
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
			/* ATPrepCmd 已确保其为表         */
			Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);
			address = ATExecDetachPartition(wqueue, tab, rel,
											((PartitionCmd *) cmd->def)->name,
											((PartitionCmd *) cmd->def)->concurrent);
			break;
		case AT_DetachPartitionFinalize:
			address = ATExecDetachPartitionFinalize(rel, ((PartitionCmd *) cmd->def)->name);
			break;
		default:				/* oops */
			elog(ERROR, "unrecognized alter table type: %d",
				 (int) cmd->subtype);
			break;
	}

	/*
	 * 将子命令报告给相关事件触发器。      
	 */
	if (cmd)
		EventTriggerCollectAlterTableSubcmd((Node *) cmd, address);

	/*
	 * 递增命令计数器，以确保序列中的下一个子命令能够 
	 * 看到至今的更改。  
	 */
	CommandCounterIncrement();
}

/*
 * ATParseTransformCmd：对一个子命令执行解析转换          
 *
 * 如有则返回转换后的子命令树，否则返回 NULL。      
 *
 * 解析器可能交回额外的 AlterTableCmd 和/或其他       
 * 工具命令，且可能位于原子命令之前或之后。       
 * 其他 AlterTableCmd 会被调度到 AlteredTableInfo 中          
 * 合适的槽位中（它们最好属于比当前更靠后的遍）。       
 * 应当发生在 AlterTableCmd 之前的工具命令会被              
 * 立即执行；而应当发生在其后的那些语句则              
 * 加入 tab->afterStmts 列表，留待最后执行。            
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

	/* 构造一个仅含该子命令和该表的 AlterTableStmt         */
	atstmt->relation =
		makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
					 pstrdup(RelationGetRelationName(rel)),
					 -1);
	atstmt->relation->inh = recurse;
	atstmt->cmds = list_make1(cmd);
	atstmt->objtype = OBJECT_TABLE; /* 此处无需苛求    */
	atstmt->missing_ok = false;

	/* 转换该 AlterTableStmt     */
	atstmt = transformAlterTableStmt(RelationGetRelid(rel),
									 atstmt,
									 context->queryString,
									 &beforeStmts,
									 &afterStmts);

	/* 执行任何应当在这些子命令之前发生的语句            */
	foreach(lc, beforeStmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		ProcessUtilityForAlterTable(stmt, context);
		CommandCounterIncrement();
	}

	/* 检查转换后的子命令并作出适当的调度                 */
	foreach(lc, atstmt->cmds)
	{
		AlterTableCmd *cmd2 = lfirst_node(AlterTableCmd, lc);
		int			pass;

		/*
		 * 这个 switch 只需覆盖可由 parse_utilcmd.c 添加的        
		 * 子命令类型；否则我们将采用默认策略，即       
		 * 立即执行该子命令作为其替代。                   
		 * （不过要注意，这确实会导致                   
		 * AT_AddConstraint 子命令被重新调度到更靠后的遍，   
		 * 这一点对于索引和外键约束很重要。）       
		 *
		 * 假定无需为新增子命令做任何 Phase 1 检查。        
		 */
		switch (cmd2->subtype)
		{
			case AT_SetNotNull:
				/* 需要针对特定命令的递归决策  */
				ATPrepSetNotNull(wqueue, rel, cmd2,
								 recurse, false,
								 lockmode, context);
				pass = AT_PASS_COL_ATTRS;
				break;
			case AT_AddIndex:
				/* 此命令不会递归       */
				/* 无需特定命令的准备     */
				pass = AT_PASS_ADD_INDEX;
				break;
			case AT_AddIndexConstraint:
				/* 此命令不会递归       */
				/* 无需特定命令的准备     */
				pass = AT_PASS_ADD_INDEXCONSTR;
				break;
			case AT_AddConstraint:
				/* 递归发生在执行阶段             */
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
				/* 此命令不会递归       */
				/* 无需特定命令的准备     */
				pass = AT_PASS_MISC;
				break;
			default:
				pass = cur_pass;
				break;
		}

		if (pass < cur_pass)
		{
			/* 无法排入我们已完成的遍               */
			elog(ERROR, "ALTER TABLE scheduling failure: too late for pass %d",
				 pass);
		}
		else if (pass > cur_pass)
		{
			/* 好，稍后排队处理  */
			tab->subcmds[pass] = lappend(tab->subcmds[pass], cmd2);
		}
		else
		{
			/*
			 * 当前遍最多应看到一个子命令，                
			 * 即原子命令转换后的版本。                        
			 */
			if (newcmd == NULL && cmd->subtype == cmd2->subtype)
			{
				/* 找到了我们子命令的转换版本         */
				newcmd = cmd2;
			}
			else
				elog(ERROR, "ALTER TABLE scheduling failure: bogus item for pass %d",
					 pass);
		}
	}

	/* 把后置语句排队留待最后执行            */
	tab->afterStmts = list_concat(tab->afterStmts, afterStmts);

	return newcmd;
}

/*
 * ATRewriteTables：阶段 3          
 */
static void
ATRewriteTables(AlterTableStmt *parsetree, List **wqueue, LOCKMODE lockmode,
				AlterTableUtilityContext *context)
{
	ListCell   *ltab;

	/* 遍历每个需要检查或重写的表                     */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);

		/* 无存储的关系在此可以忽略          */
		if (!RELKIND_HAS_STORAGE(tab->relkind))
			continue;

		/*
		 * 若我们更改列数据类型，该操作必须传播到         
		 * 那些把本表的行类型用作列类型的表。      
		 * 在为表增加带默认值的列时，tab->newvals 也            
		 * 会是非 NULL。我们同样选择禁止这种情况，      
		 * 因为复合类型将来也可能支持默认值。     
		 *
		 * （最终我们可能还需在不重写的情况下        
		 * 就检查复合类型依赖，但目前复合类型不强制  
		 * 执行任何约束，因此仅在 ALTER 期间强制它们既   
		 * 无必要也不合适。）                                        
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
		 * 只有当至少有一列需要重新计算，或者我们要更  
		 * 改表的持久性或访问方法时，才需要重写表。       
		 *
		 * 更改持久性需重写的原因有两个：一方面，  
		 * 我们要确保两个关系各自的缓冲区都正确地   
		 * 标记为带或不带 BM_PERMANENT；另一方面，由于       
		 * 重写会创建并分配新的 relfilenumber，我们会自动    
		 * 为该关系创建或删除合适的 init 分支。                
		 *                                           
		 */
		if (tab->rewrite > 0 && tab->relkind != RELKIND_SEQUENCE)
		{
			/* 构建临时关系并复制数据        */
			Relation	OldHeap;
			Oid			OIDNewHeap;
			Oid			NewAccessMethod;
			Oid			NewTableSpace;
			char		persistence;

			OldHeap = table_open(tab->relid, NoLock);

			/*
			 * 我们不支持重写系统目录；边角情况太多，   
			 * 而收益太小。特别是对映射目录而言，这样做 
			 * 肯定行不通。                                 
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
			 * 不允许重写其他后端的临时表……它们的本地  
			 * 缓冲区管理器无法应付。（与 CheckAlterTableIsSafe 
			 *  中的检查重复，但为保险起见，我们在此也   
			 * 做检查。） 
			 */
			if (RELATION_IS_OTHER_TEMP(OldHeap))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot rewrite temporary tables of other sessions")));

			/*
			 * 选择目标表空间（若用户未要求更改则同     
			 * 于原表）       
			 */
			if (tab->newTableSpace)
				NewTableSpace = tab->newTableSpace;
			else
				NewTableSpace = OldHeap->rd_rel->reltablespace;

			/*
			 * 选择目标访问方法（若用户未要求更改则同     
			 * 于原表）       
			 */
			if (OidIsValid(tab->newAccessMethod))
				NewAccessMethod = tab->newAccessMethod;
			else
				NewAccessMethod = OldHeap->rd_rel->relam;

			/*
			 * 为瞬态表选择持久性（若用户未要求更改则同  
			 * 于原表）            
			 */
			persistence = tab->chgPersistence ?
				tab->newrelpersistence : OldHeap->rd_rel->relpersistence;

			table_close(OldHeap, NoLock);

			/*
			 * 在实际重写表之前，此刻就触发事件触发器   
			 * 。   
			 *
			 * 嵌套命令在任何位置都不支持事件触发器，   
			 * 此处也不例外；当来自 AlterTableInternal 时，   
			 * parsetree 为 NULL 
			 *
			 * 且只触发一次。 
			 */
			if (parsetree)
				EventTriggerTableRewrite((Node *) parsetree,
										 tab->relid,
										 tab->rewrite);

			/*
			 * 创建用于接收修改后数据的瞬态表。           
			 *
			 * 确保其正确标记为永久或非永久。我们必须在 
			 * 在此完成此事，让新 relfilenumber 的缓冲区具有  
			 * 正确的持久性设置，同时确保原有缓冲区会  
			 * 以正确的设置（即原设置）读入。否则，重写 
			 * 之后的回滚可能使原有文件编号的缓冲区   
			 * 带有错误的持久性设                                  
			 * 置。                                                   
			 *         
			 *
			 * 注意：swap_relation_files() 也会交换持久性。   
			 * 这对 pg_class 不适用，它不可能是非永久的。     
			 *                 
			 */
			OIDNewHeap = make_new_heap(tab->relid, NewTableSpace, NewAccessMethod,
									   persistence, lockmode);

			/*
			 * 把堆数据连同所需修改复制进新表，并   
			 * 测试表中现有数据是否满足 ALTER TABLE 命令  
			 * 生成的新约束。                                     
			 */
			ATRewriteTable(tab, OIDNewHeap, lockmode);

			/*
			 * 交换新旧堆的物理文件，然后重建索引并丢弃  
			 * 旧堆。我们可以将 RecentXmin 用作表的新         
			 * relfrozenxid，因为所有元组都已在 ATRewriteTable     
			 * 中重写过，表中不会再残留更早的 XID。此外，  
			 * 我们从不按内容交换 TOAST 表，因为我们无意让 
			 * 这段代码用于系统目录。                     
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
			 * 如果需要，则测试表中现有数据是否满足         
			 * ALTER TABLE 命令生成的新约束，但不重建     
			 * 数据。    
			 */
			if (tab->constraints != NIL || tab->verify_new_notnull ||
				tab->partition_constraint != NULL)
				ATRewriteTable(tab, InvalidOid, lockmode);

			/*
			 * 如果我们用了 SET TABLESPACE 但无需重构元组，   
			 * 只需逐块复制即可。   
			 */
			if (tab->newTableSpace)
				ATExecSetTableSpace(tab->relid, tab->newTableSpace, lockmode);
		}

		/*
		 * 同时更改所属序列的持久性，使其与表的持久性   
		 * 保持一致。   
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
	 * 外键约束放在最后一遍检查，因为 (a) 最好逐个     
	 * 检查每个约束；(b) 我们至少在理论上可能已经更改 
	 * 了外键双方的关系，因此在尝试读取表之前最好  
	 * 已经完成了双方的重写。                                     
	 *                    
	 */
	foreach(ltab, *wqueue)
	{
		AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
		Relation	rel = NULL;
		ListCell   *lcon;

		/* 无存储的关系在此同样可以忽略        */
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
					/* 早已加锁，无需再加锁         */
					rel = table_open(tab->relid, NoLock);
				}

				refrel = table_open(con->refrelid, RowShareLock);

				validateForeignKeyConstraint(fkconstraint->conname, rel, refrel,
											 con->refindid,
											 con->conid);

				/*
				 * 无需将约束行标记为已验证，此前插入时 
				 * 已经做过了。                    
				 */

				table_close(refrel, NoLock);
			}
		}

		if (rel)
			table_close(rel, NoLock);
	}

	/*  最后执行任何已排队的 afterStmts      */
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
 * ATRewriteTable：扫描或重写表      
 *
 * 无需重写时 OIDNewHeap 为 InvalidOid           
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
	 * 打开关系。我们肯定已经锁定了现有的表。        
	 *       
	 */
	oldrel = table_open(tab->relid, NoLock);
	oldTupDesc = tab->oldDesc;
	newTupDesc = RelationGetDescr(oldrel);	/*  含全部修改  */

	if (OidIsValid(OIDNewHeap))
		newrel = table_open(OIDNewHeap, lockmode);
	else
		newrel = NULL;

	/*
	 * 为 table_tuple_insert 准备 BulkInsertState 和选项。FSM         
	 * 为空，不必费心使用它。  
	 */
	if (newrel)
	{
		mycid = GetCurrentCommandId(true);
		bistate = GetBulkInsertState();
		ti_options = TABLE_INSERT_SKIP_FSM;
	}
	else
	{
		/*  免得编译器警告用到未初始化的变量   */
		mycid = 0;
		bistate = NULL;
		ti_options = 0;
	}

	/*
	 * 生成约束和默认值的执行状态             
	 */

	estate = CreateExecutorState();

	/*  构建所需的表达式执行状态        */
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
				/*  此处无操作   */
				break;
			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	/*  为分区检查条件构建表达式执行状态           */
	if (tab->partition_constraint)
	{
		needscan = true;
		partqualstate = ExecPrepareExpr(tab->partition_constraint, estate);
	}

	foreach(l, tab->newvals)
	{
		NewColumnValue *ex = lfirst(l);

		/*  表达式已规划  */
		ex->exprstate = ExecInitExpr((Expr *) ex->expr, NULL);
	}

	notnull_attrs = NIL;
	if (newrel || tab->verify_new_notnull)
	{
		/*
		 * 如果要重建元组，或者添加了任何新的但尚未  
		 * 验证的 NOT NULL 约束，则检查所有非空约束。这      
		 * 有点过度，但能把出错风险降到最低，且 
		 * heap_attisnull 反正是廉价的测试。   
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
			 * 元组或页面上的所有谓词锁都即将失效，         
			 * 因为我们会移动元组。将它们提升为        
			 * 关系锁。   
			 */
			TransferPredicateLocksToHeapRelation(oldrel);
		}

		econtext = GetPerTupleExprContext(estate);

		/*
		 * 创建必要的元组槽。重写时需要两个槽，             
		 * 否则一个即可。在一个槽就够的情况下，我们   
		 * 需要使用新的元组描述符，否则某些约束          
		 * 将无法被求值。注意，即使元组布局相同且不    
		 * 需要重写，tupDesc 也可能不同                    
		 * （比如 ADD COLUMN 无默认值）。 
		 */
		if (tab->rewrite)
		{
			Assert(newrel != NULL);
			oldslot = MakeSingleTupleTableSlot(oldTupDesc,
											   table_slot_callbacks(oldrel));
			newslot = MakeSingleTupleTableSlot(newTupDesc,
											   table_slot_callbacks(newrel));

			/*
			 * 先将新槽中的所有列置为 NULL，以确保          
			 * 重写中添加的列被初始化为 NULL。这              
			 * 是必要的，因为 tab->newvals 不会            
			 * 包含 NULL 默认值列的表达式，比如添加           
			 * 无默认值的列同时添加带有默认值、              
			 * 需要实际重写的列。 
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
		 * 根据新元组描述符被删除的属性                
		 * 可以置为 NULL。我们预先计算出已删除              
		 * 属性的列表，以免在每元组循环中重复做。  
		 */
		for (i = 0; i < newTupDesc->natts; i++)
		{
			if (TupleDescAttr(newTupDesc, i)->attisdropped)
				dropped_attrs = lappend_int(dropped_attrs, i);
		}

		/*
		 * 扫描各行，在需要时生成一个新行，然后        
		 * 检查所有约束。        
		 */
		snapshot = RegisterSnapshot(GetLatestSnapshot());
		scan = table_beginscan(oldrel, snapshot, 0, NULL);

		/*
		 * 切换到每元组内存上下文，并为产生的每个     
		 * 元组重置以免泄漏内存。 
		 */
		oldCxt = MemoryContextSwitchTo(GetPerTupleMemoryContext(estate));

		while (table_scan_getnextslot(scan, ForwardScanDirection, oldslot))
		{
			TupleTableSlot *insertslot;

			if (tab->rewrite > 0)
			{
				/*  从旧元组提取数据   */
				slot_getallattrs(oldslot);
				ExecClearTuple(newslot);

				/*  复制属性   */
				memcpy(newslot->tts_values, oldslot->tts_values,
					   sizeof(Datum) * oldslot->tts_nvalid);
				memcpy(newslot->tts_isnull, oldslot->tts_isnull,
					   sizeof(bool) * oldslot->tts_nvalid);

				/*  将已删除属性在新元组中置为空 */
				foreach(lc, dropped_attrs)
					newslot->tts_isnull[lfirst_int(lc)] = true;

				/*
				 * 约束和 GENERATED 表达式可能会引用             
				 * tableoid 列，所以要将所需的值填入         
				 * tts_tableOid。（每次都必须这样做，      
				 * 存储时会被 newrel 的 OID 覆盖。）   
				 */
				newslot->tts_tableOid = RelationGetRelid(oldrel);

				/*
				 * 处理提供的表达式以替换选定的列。         
				 *
				 * 首先，求值那些输入来自旧元组的表达式    
				 * 。   
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
				 * 接着，求值输入来自新元组的那些表达式  
				 * 。我们假设这些列不会相互引用，           
				 * 因此不存在顺序依赖。                
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
				 * 如果没有重写，则新旧表保证具有             
				 * 相同的 AM，因此可以直接用旧槽来验证        
				 * 新约束等。     
				 */
				insertslot = oldslot;
			}

			/*  现在检查可能已变更元组上的所有约束    */
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
						/*  此处无操作   */
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

			/*  将元组写出到新关系中         */
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
 * ATGetQueueEntry：ALTER TABLE 工作队列中查找或创建条目    
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
	 * 没找到，所以添加它。注意，在任何有趣的事情  
	 * 发生之前，我们会先复制关系的现有描述符。     
	 */
	tab = (AlteredTableInfo *) palloc0(sizeof(AlteredTableInfo));
	tab->relid = relid;
	tab->rel = NULL;			/*  待设   */
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
			return NULL;		/*  非真实语法 */
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
			return NULL;		/*  非真实语法 */
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
			return NULL;		/*  非真实语法 */
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
			return NULL;		/*  非真实语法 */
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
			return NULL;		/*  非真实语法 */
	}

	return NULL;
}

/*
 * ATSimplePermissions
 *
 * - 确保它是关系（或可能是视图）       
 * - 确保该用户是所有者  
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

	/*  目标类型错？*/
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
			/*  内部错误？*/
			elog(ERROR, "invalid ALTER action attempted on relation \"%s\"",
				 RelationGetRelationName(rel));
	}

	/*  权限检查      */
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
 * 简单表递归，足以满足大多数 ALTER TABLE 操作。     
 * 所有直接和间接子表都以未指定的顺序进行处理。     
 * 注意，如果一个子表通过多条继承路径从原始表   
 * 继承而来，它只会被访问一次。      
 */
static void
ATSimpleRecursion(List **wqueue, Relation rel,
				  AlterTableCmd *cmd, bool recurse, LOCKMODE lockmode,
				  AlterTableUtilityContext *context)
{
	/*
	 * 若需要且存在（或可能有）子表，则向子表传播     
	 * 。      
	 */
	if (recurse && rel->rd_rel->relhassubclass)
	{
		Oid			relid = RelationGetRelid(rel);
		ListCell   *child;
		List	   *children;

		children = find_all_inheritors(relid, lockmode, NULL);

		/*
		 * find_all_inheritors 对继承层次结构做递归搜索，     
		 * 所以我们只需处理它返回的列表中的所有             
		 * relid。             
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirst_oid(child);
			Relation	childrel;

			if (childrelid == relid)
				continue;
			/*  find_all_inheritors 已获得锁    */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);
			ATPrepCmd(wqueue, childrel, cmd, false, true, lockmode, context);
			relation_close(childrel, NoLock);
		}
	}
}

/*
 * 获取给定表的分区列表，以给定的锁模式锁定它们，      
 * 并确保它们全部通过 CheckAlterTableIsSafe。           
 *
 * 如果给定关系不是分区表，则此函数为空操作；           
 * 特别是，如果它是传统继承父表，则什么也不做。 
 */
static void
ATCheckPartitionsNotInUse(Relation rel, LOCKMODE lockmode)
{
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		List	   *inh;
		ListCell   *cell;

		inh = find_all_inheritors(RelationGetRelid(rel), lockmode, NULL);
		/*  首个元素是父 rel；必须忽略它       */
		for_each_from(cell, inh, 1)
		{
			Relation	childrel;

			/*  find_all_inheritors 已获得锁    */
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
 * 将 ALTER TYPE 操作传播到该类型的类型表。           
 * 同时检查 RESTRICT/CASCADE 行为。给定 CASCADE 时，         
 * 还允许递归到类型表的继承子表。         
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
 * find_composite_type_dependencies
 *
 * 检查类型 "typeOid" 是否正被用作某个表中的列               
 * （可能嵌套在组合类型、数组等中好几层深！）        
 * 最终，我们希望将检查或重写操作传播到           
 * 这类表中，但目前，如果发现任何就报错。    
 *
 * 调用者应提供行类型的关联关系，或者提供一      
 * 个类型名（二选一）用于错误消息（如有）。   
 *
 * 注意，"typeOid" 不一定是组合类型；它也可以是            
 * 另一种容器类型，比如数组或范围，或者是                
 * 这些类型之上的域。因此这个函数名是历史遗留，        
 * 但改掉它不值得。    
 *
 * 我们假设依赖该类型的函数和视图并不构成               
 * 拒绝 ALTER 的理由。（这安全吗？）  
 */
void
find_composite_type_dependencies(Oid typeOid, Relation origRelation,
								 const char *origTypeName)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc depScan;
	HeapTuple	depTup;

	/*  由于此函数会递归，它可能被驱动到栈溢出         */
	check_stack_depth();

	/*
	 * 我们扫描 pg_depend 来查找依赖给定类型的东西。       
	 * （假设类型可忽略 refobjsubid。）       
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

		/*  检查直接依赖的类型       */
		if (pg_depend->classid == TypeRelationId)
		{
			/*
			 * 这一定是包含给定类型的数组、域或范围，   
			 * 所以要递归检查此类型的使用。注意，         
			 * 任何错误消息都会提到原始类型而不是     
			 * 容器；这是有意的。    
			 */
			find_composite_type_dependencies(pg_depend->objid,
											 origRelation, origTypeName);
			continue;
		}

		/*  否则，忽略非关系的依赖对象     */
		if (pg_depend->classid != RelationRelationId)
			continue;

		rel = relation_open(pg_depend->objid, AccessShareLock);
		tupleDesc = RelationGetDescr(rel);

		/*
		 * 如果 objsubid 标识了一个特定列，则在错误         
		 * 消息中提及该列。否则，搜索是否存在                
		 * 该类型的用户列。（我们假设系统列不会           
		 * 有值得关注的类型。）之所以需要这个搜索，  
		 * 因为包含目标类型表达式列的索引只会被             
		 * 记录为整个关系的依赖。如果没有找到                
		 * 该类型的列，则该依赖必定表明：该类型           
		 * 只是被索引表达式临时引用而未存储在              
		 * 磁盘上；我们假设这没问题，正如视图中           
		 * 的引用。（也可能目标类型内嵌在索引列存储      
		 * 的容器类型中，这应由前面的递归捕获。）   
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
				/*  无此列，故假定无碍 */
				relation_close(rel, AccessShareLock);
				continue;
			}
		}

		/*
		 * 如果关系有存储，我们肯定应该拒绝。如果它     
		 * 是分区表，那么或许不必拒绝：如果有分区，   
		 * 那么发现其中一个时我们就会失败，否则就    
		 * 没有需要担心的已存储数据。但是，该类型        
		 * 的变更可能会影响关于该类型是否可排序            
		 * 或可哈希的结论，从而（如果它是分区列）     
		 * 破坏分区规则。目前对分区关系也拒绝。       
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
			 * 视图或组合类型本身不是问题，但我们必须   
			 * 通过其行类型递归检查间接依赖。               
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
 * 检查组合类型是否正被用作某个类型表的         
 * 类型。若行为是 RESTRICT 且找到表，则中止。    
 * 否则返回表列表。       
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
 * check_of_type
 *
 * 检查一个类型是否适用于 CREATE TABLE OF/ALTER TABLE OF。如果  
 * 不适用，则抛出错误。目前，我们要求该类型        
 * 源自 CREATE TYPE AS。我们可以支持任何行类型，                
 * 但这样做需要在 DDL 命令中处理许多额外的边角情况。   
 * （此外，允许组合类型之上的域将引出                     
 * 一堆麻烦：域的约束是否以及如何应用于派生表。）    
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
		 * 关闭父关系，但在事务提交之前保持其上的          
		 * AccessShareLock。这将防止其他人在类型表              
		 * 的创建/转换提交之前删除或 ALTER 该类型。    
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
 * 更改表添加列    
 *
 * 向关系添加一个附加属性，其假设 CHECK、NOT NULL       
 * 和 FOREIGN KEY 约束将被 parse_utilcmd.c 从                     
 * AT_AddColumn AlterTableCmd 中移除，并作为独立                
 * 命令加入。 
 *
 * ADD COLUMN 无法使用普通的 ALTER TABLE 递归机制，因为          
 * 我们必须在运行时决定是否递归，这取决于我们            
 * 究竟是实际添加一列，还是仅仅与现有列合并。          
 * （我们无法在静态预检查中确认，因为它无法正确          
 * 处理多继承。） 
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
 * 向表添加一列。返回值是新列在父关系中的       
 * 地址。                         
 *
 * cmd 是按引用传递的，这样我们可以用解析转换          
 * 副本替换它（但只在检查 IF NOT EXISTS 之后）。  
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

	/*  由于此函数会递归，它可能被驱动到栈溢出         */
	check_stack_depth();

	/*  顶层权限检查已在 ATPrepCmd 完成，否则在此做     */
	if (recursing)
		ATSimplePermissions((*cmd)->subtype, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	if (rel->rd_rel->relispartition && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot add column to a partition")));

	attrdesc = table_open(AttributeRelationId, RowExclusiveLock);

	/*
	 * 我们是在向递归子表添加列吗？如果是，检查是否     
	 * 与该列的现有定义合并。若合并，则不允许            
	 * 再递归。子表已有该列，再递归会搞乱                    
	 * attinhcount。                      
	 */
	if (colDef->inhcount > 0)
	{
		HeapTuple	tuple;

		/*  子表是否已有该名称的列？          */
		tuple = SearchSysCacheCopyAttName(myrelid, colDef->colname);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_attribute childatt = (Form_pg_attribute) GETSTRUCT(tuple);
			Oid			ctypeId;
			int32		ctypmod;
			Oid			ccollid;

			/*  子列必须匹配类型、typmod 和排序规则     */
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

			/*  递增现有子属性的 inhcount     */
			childatt->attinhcount++;
			if (childatt->attinhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
			CatalogTupleUpdate(attrdesc, &tuple->t_self, tuple);

			heap_freetuple(tuple);

			/*  向用户告知这次合并    */
			ereport(NOTICE,
					(errmsg("merging definition of column \"%s\" for child \"%s\"",
							colDef->colname, RelationGetRelationName(rel))));

			table_close(attrdesc, RowExclusiveLock);

			/*  使子列的变更可见            */
			CommandCounterIncrement();

			return InvalidObjectAddress;
		}
	}

	/*  如果名称已存在且 if_not_exists 为真，则跳过*/
	if (!check_for_column_name_collision(rel, colDef->colname, if_not_exists))
	{
		table_close(attrdesc, RowExclusiveLock);
		return InvalidObjectAddress;
	}

	/*
	 * 好，我们需要添加列，所以继续进行解析   
	 * 转换。这可能导致排队、甚至立即执行                 
	 * 附属操作（例如创建唯一索引）；                         
	 * 所以我们必须等到完成 if_not_exists 检查之后。   
	 *
	 * 递归时，命令已经转换过，我们不必再转换。         
	 * 此外，如果没有给出上下文，我们无法转换。      
	 * （目前这只发生在 AT_AddColumnToView；我们期望          
	 * view.c 传入的 ColumnDef 无需加工。） 
	 */
	if (context != NULL && !recursing)
	{
		*cmd = ATParseTransformCmd(wqueue, tab, rel, *cmd, recurse, lockmode,
								   cur_pass, context);
		Assert(*cmd != NULL);
		colDef = castNode(ColumnDef, (*cmd)->def);
	}

	/*
	 * 如果表有子表，则无法添加标识列，因为标识           
	 * 不会被继承。（分开添加列和标识则可行。）    
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

	/*  确定新属性的编号            */
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

	/*  确保数据类型作为列是合法的 */
	CheckAttributeType(colDef->colname, typeOid, collOid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	/*
	 * 构造新的 pg_attribute 条目。（变长字段由                 
	 * InsertPgAttributeTuples() 处理。）    
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
	 * 酌情更新 pg_class 元组        
	 */
	((Form_pg_class) GETSTRUCT(reltup))->relnatts = newattnum;

	CatalogTupleUpdate(pgclass, &reltup->t_self, reltup);

	heap_freetuple(reltup);

	/*  新属性的创建后钩子         */
	InvokeObjectPostCreateHook(RelationRelationId, myrelid, newattnum);

	table_close(pgclass, RowExclusiveLock);

	/*  使该属性的系统表条目可见      */
	CommandCounterIncrement();

	/*
	 * 若有 DEFAULT 则存入系统表         
	 */
	if (colDef->raw_default)
	{
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attribute.attnum;
		rawEnt->raw_default = copyObject(colDef->raw_default);
		rawEnt->missingMode = false;	/*  XXX: 残留  */
		rawEnt->generated = colDef->generated;

		/*
		 * 此函数本用于 CREATE TABLE，因此它处理一个      
		 * 默认值 _list_，只做一个。     
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);

		/*  使其他系统表变更可见             */
		CommandCounterIncrement();
	}

	/*
	 * 告诉 Phase 3 填入默认值表达式（如果有的话）。 
	 *
	 * 如果没有默认值，Phase 3 无事可做，因为               
	 * 那实际上就意味着默认值是 NULL。堆元组访问            
	 * 例程总是检查 attnum 是否大于元组中的属性数，         
	 * 如果是则返回 NULL，所以无需修改元组数据，          
	 * 就能得到新列为 NULL 的效果。     
	 *
	 * 例外情况是新列的类型为域类型：域可能                 
	 * 带有 NOT NULL 约束，或带有间接拒绝空值的                
	 * 检查约束。如果存在任何域约束，则我们               
	 * 构造一个显式的 NULL 默认值，它将经过       
	 * CoerceToDomain 处理。（这有点低效，因为                    
	 * 它会导致重写表，而我们其实本不必这么做；         
	 * 但我们这么做是为了保持历史行为，即只有             
	 * 表中当前有行时才引发该失败。）   
	 *
	 * 注意：我们使用 build_column_default，而不只是         
	 * AddRelationNewConstraints 返回的已加工默认值，              
	 * 以便默认值适用时正确。 
	 *
	 * 注意：看似这应该发生在 Phase 2 的末尾，以便         
	 * 后续子命令的效果能被考虑在内。不过，我们         
	 * 现在就做是有意的。新列应当按照 ADD COLUMN             
	 * 子命令中所说的来填充，这样效果就如同该子          
	 * 命令被单独运行、后续子命令在新的 ALTER TABLE         
	 * 命令中发出时一样。                                           
	 *
	 * 对于没有存储的关系，我们完全可以跳过这一步，    
	 * 因为 Phase 3 肯定不会碰它们。系统属性也没有       
	 * 值得关注的默认值。  
	 */
	if (RELKIND_HAS_STORAGE(relkind) && attribute.attnum > 0)
	{
		bool		has_domain_constraints;
		bool		has_missing = false;

		/*
		 * 对标识列，无法使用 build_column_default()，       
		 * 因为序列所有权尚未设置。所以要手工完成。     
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

		/*  需要时构建 CoerceToDomain(NULL) 表达式 */
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
			if (defval == NULL) /*  不应发生     */
				elog(ERROR, "failed to coerce base type to domain");
		}

		if (defval)
		{
			NewColumnValue *newval;

			/*  准备 defval 以供执行，或在此处或在 Phase 3*/
			defval = expression_planner(defval);

			/*  将新默认值加入 newvals 列表   */
			newval = (NewColumnValue *) palloc0(sizeof(NewColumnValue));
			newval->attnum = attribute.attnum;
			newval->expr = defval;
			newval->is_generated = (colDef->generated != '\0');

			tab->newvals = lappend(tab->newvals, newval);

			/*
			 * 尝试通过把指定的 DEFAULT 值存于堆外来    
			 * 跳过完整的表重写。这只允许用于普通关系 
			 * 和非生成列，且默认值表达式无法是易失的     
			 * （稳定则可以）。注意，                              
			 * contain_volatile_functions 认为 CoerceToDomain              
			 * 是不可变的，但本处认为向带约束域的强制     
			 * 转换是易失的；否则表空时也可能失败。    
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

				/*  求值默认值表达式       */
				estate = CreateExecutorState();
				exprState = ExecPrepareExpr(defval, estate);
				missingval = ExecEvalExpr(exprState,
										  GetPerTupleExprContext(estate),
										  &missingIsNull);
				/*  若结果为 NULL 则无事可做；否则存储  */
				if (!missingIsNull)
				{
					StoreAttrMissingVal(rel, attribute.attnum, missingval);
					/*  使该额外的系统表变更可见      */
					CommandCounterIncrement();
					has_missing = true;
				}
				FreeExecutorState(estate);
			}
			else
			{
				/*
				 * 使用 missing 模式失败。我们必须重写表       
				 * 来安装该值。   
				 */
				tab->rewrite |= AT_REWRITE_DEFAULT_VAL;
			}
		}

		if (!has_missing)
		{
			/*
			 * 如果新列是 NOT NULL，且没有缺失值，             
			 * 则告知 Phase 3 需要检查 NULL。   
			 */
			tab->verify_new_notnull |= colDef->is_not_null;
		}
	}

	/*
	 * 为新列添加所需的依赖条目。          
	 */
	add_column_datatype_dependency(myrelid, newattnum, attribute.atttypid);
	add_column_collation_dependency(myrelid, newattnum, attribute.attcollation);

	/*
	 * 酌情向子表传播。与大多数其他 ALTER 例程       
	 * 不同，我们必须一次做一层递归；我们无法              
	 * 用 find_all_inheritors 一次完成。      
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * 如果被告知不要递归，那么最好没有任何子       
	 * 表；否则添加会让它们步调不一。        
	 */
	if (children && !recurse)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("column must be added to child tables too")));

	/*  子表看到的列应是单重继承          */
	if (!recursing)
	{
		childcmd = copyObject(*cmd);
		colDef = castNode(ColumnDef, childcmd->def);
		colDef->inhcount = 1;
		colDef->is_local = false;
	}
	else
		childcmd = *cmd;		/*  无需再次复制   */

	foreach(child, children)
	{
		Oid			childrelid = lfirst_oid(child);
		Relation	childrel;
		AlteredTableInfo *childtab;

		/*  find_inheritance_children 已获得锁    */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/*  为该表查找或创建工作队列条目    */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/*  递归到子表；返回值被忽略     */
		ATExecAddColumn(wqueue, childtab, childrel,
						&childcmd, recurse, true,
						lockmode, cur_pass, context);

		table_close(childrel, NoLock);
	}

	ObjectAddressSubSet(address, RelationRelationId, myrelid, newattnum);
	return address;
}

/*
 * 如果新的或重命名的列将与现有列名冲突，           
 * 且 if_not_exists 为假，则报错；否则不做任何事。   
 */
static bool
check_for_column_name_collision(Relation rel, const char *colname,
								bool if_not_exists)
{
	HeapTuple	attTuple;
	int			attnum;

	/*
	 * 这个测试故意不感知 attisdropped，因为若有                
	 * 人尝试添加与已删除列名匹配的列，反正会失败。  
	 */
	attTuple = SearchSysCache2(ATTNAME,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   PointerGetDatum(colname));
	if (!HeapTupleIsValid(attTuple))
		return true;

	attnum = ((Form_pg_attribute) GETSTRUCT(attTuple))->attnum;
	ReleaseSysCache(attTuple);

	/*
	 * 对于与系统列名的冲突，我们抛出不同的             
	 * 错误消息，因为它们通常不显示，否则用户            
	 * 可能会对冲突的原因感到困惑。    
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
 * 为该列建立对其数据类型的依赖。 
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
 * 为该列建立对其排序规则的依赖。  
 */
static void
add_column_collation_dependency(Oid relid, int32 attnum, Oid collid)
{
	ObjectAddress myself,
				referenced;

	/*  我们知道默认排序规则已固定，不必费心记录它      */
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
 * 更改表更改列移除 NOT NULL     
 */

static void
ATPrepDropNotNull(Relation rel, bool recurse, bool recursing)
{
	/*
	 * 如果父表是分区表，那么与检查约束一样，我们        
	 * 不支持在存在分区时移除 NOT NULL。        
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
 * 返回被修改列的地址。如果该列本来就可空，         
 * 则返回 InvalidObjectAddress。          
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
	 * 查找该属性     
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

	/*  防止用户修改系统属性               */
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
	 * 检查该属性是否不在主键或被用作复制标识              
	 * 的索引中。    
	 *
	 * 注意：即使主键索引无效，我们也会报错。   
	 */

	/*  遍历该关系上的所有索引    */
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
		 * 如果该索引既非主键，也非被用作复制标识的 
		 * 索引则跳过检查。 
		 */
		if (indexStruct->indisprimary || indexStruct->indisreplident)
		{
			/*
			 * 遍历主键或被用作复制标识的索引中的          
			 * 每个属性，看它是否与待修改的属性           
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

	/*  若 rel 是分区，父表有相同约束时不应删除 NOT NULL  */
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
	 * 好，需要时才实际执行系统表变更。       
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
 * 更改表更改列设置 NOT NULL    
 */

static void
ATPrepSetNotNull(List **wqueue, Relation rel,
				 AlterTableCmd *cmd, bool recurse, bool recursing,
				 LOCKMODE lockmode, AlterTableUtilityContext *context)
{
	/*
	 * 如果我们已经在递归中，则无事可做；最顶层  
	 * 的 ATSimpleRecursion 调用已经访问过所有子表。   
	 */
	if (recursing)
		return;

	/*
	 * 如果目标列已被标记为 NOT NULL，我们可以跳过          
	 * 对子表的递归，因为它们的列应该已经被标记           
	 * 为 NOT NULL。但除非该关系有一些子表，                 
	 * 否则我们可以等到执行时再检查。（如果它            
	 * 确实有子表，那么这可以避免不必要地获取        
	 * 每子表锁。这在某些并行恢复场景中大大改         
	 * 善并发。）    
	 *
	 * 不幸的是，我们只能将此优化应用于分区表，     
	 * 因为传统继承不会强制子列在父列为 NOT NULL         
	 * 时也为 NOT NULL。（这是一个将来某天应                 
	 * 修复的缺陷） 
	 */
	if (rel->rd_rel->relhassubclass &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		HeapTuple	tuple;
		bool		attnotnull;

		tuple = SearchSysCacheAttName(RelationGetRelid(rel), cmd->name);

		/*  如果名称有误，不妨现在就抛出错误 */
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
	 * 如果我们在分区表上有 ALTER TABLE ONLY ... SET NOT NULL， 
	 *  则对每个子表应用 ALTER TABLE ... CHECK NOT NULL。         
	 * 否则普通递归。  
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
 * 返回被修改列的地址。如果该列本来就是 NOT               
 *  NULL，返回 InvalidObjectAddress。 
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
	 * 查找该属性     
	 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	attnum = ((Form_pg_attribute) GETSTRUCT(tuple))->attnum;

	/*  防止用户修改系统属性               */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * 好，需要时才实际执行系统表变更。       
	 */
	if (!((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull)
	{
		((Form_pg_attribute) GETSTRUCT(tuple))->attnotnull = true;

		CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

		/*
		 * 通常，Phase 3 必须确保被设置为 NOT NULL 的列中      
		 * 不存在 NULL。但是，如果我们能找到证明这一        
		 * 点的约束，则可跳过这一步。如果已经            
		 * 发现必须验证其他 NOT NULL 约束，就无需再找。    
		 */
		if (!tab->verify_new_notnull &&
			!NotNullImpliedByRelConstraints(rel, (Form_pg_attribute) GETSTRUCT(tuple)))
		{
			/*  告知 Phase 3 它需要测试该约束     */
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
 * 更改表更改列检查 NOT NULL      
 *
 * 这在语法中并不存在，但如果用户在分区表上      
 * 写入 ALTER TABLE ONLY ... SET NOT NULL，或者                 
 * 尝试在其上创建主键（其内部会在分区表上         
 * 创建 AT_SetNotNull），那么我们会生成针对其分区  
 * 的 AT_CheckNotNull 命令。这样的命令不允许            
 * 我们实际修改任何分区，但如果分区已经正确    
 * 标记，我们希望让它通过。                     
 *
 * 将来，这可能需要调整子表的状态，可能是           
 * 通过为 attnotnull 约束增加继承计数。目前             
 * 我们只需检查该标志是否存在。              
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
 * NotNullImpliedByRelConstraints
 *		rel 的现有约束是否蕴含给定属性的 NOT NULL？             
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
	 * argisrow = false 即使对组合列也正确，因为此时     
	 * attnotnull 并不表示 SQL 标准的 IS NOT NULL 测试，        
	 * 只是 IS DISTINCT FROM NULL。  
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
 * 更改表更改列设置/移除默认值 
 *
 * 返回受影响列的地址。            
 */
static ObjectAddress
ATExecColumnDefault(Relation rel, const char *colName,
					Node *newDefault, LOCKMODE lockmode)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	AttrNumber	attnum;
	ObjectAddress address;

	/*
	 * 取得该属性的编号       
	 */
	attnum = get_attnum(RelationGetRelid(rel), colName);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));

	/*  防止用户修改系统属性               */
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
	 * 移除该列的任何旧默认值。这里用 RESTRICT          
	 * 以求安全，但目前我们不期望有任何东西依赖     
	 * 它。  
	 *
	 * 当移除现有默认值是为添加新默认值做准备的           
	 * 时候，我们将其视为内部操作；而当用户要求   
	 * 删除时，为用户发起的操作。  
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  newDefault != NULL);

	if (newDefault)
	{
		/*  SET DEFAULT*/
		RawColumnDefault *rawEnt;

		rawEnt = (RawColumnDefault *) palloc(sizeof(RawColumnDefault));
		rawEnt->attnum = attnum;
		rawEnt->raw_default = newDefault;
		rawEnt->missingMode = false;
		rawEnt->generated = '\0';

		/*
		 * 此函数本用于 CREATE TABLE，因此它处理一个      
		 * 默认值 _list_，只做一个。     
		 */
		AddRelationNewConstraints(rel, list_make1(rawEnt), NIL,
								  false, true, false, NULL);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * 加入预制默认值表达式。   
 *
 * 返回受影响列的地址。            
 */
static ObjectAddress
ATExecCookedColumnDefault(Relation rel, AttrNumber attnum,
						  Node *newDefault)
{
	ObjectAddress address;

	/*  假设无需任何检查         */

	/*
	 * 移除该列的任何旧默认值。这里用 RESTRICT          
	 * 以求安全，但目前我们不期望有任何东西依赖     
	 * 该默认值。（在普通情况下，本来不可能有          
	 * 默认值，但在将 LIKE 与继承组合时则有可能。） 
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, false,
					  true);

	(void) StoreAttrDefault(rel, attnum, newDefault, true, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * 更改表更改列添加标识       
 *
 * 返回受影响列的地址。            
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

	/*  无法修改系统属性      */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * 将列创建为标识列隐含着 NOT NULL，因此向一个          
	 * 不是 NOT NULL 的现有列添加标识，会造成一种          
	 * 不经曲折就无法重现的状态。  
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
 * ALTER TABLE ALTER COLUMN SET { GENERATED or sequence options }
 *
 * 返回受影响列的地址。            
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
	 * 即使这里没有什么要更改，我们也运行全部检          
	 * 查。后续会有一个 ALTER SEQUENCE 依赖于这一切         
	 * 。   
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
 * 更改表更改列移除标识        
 *
 * 返回受影响列的地址。            
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

	/*  删除内部序列        */
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
 * 更改表更改列移除表达式       
 */
static void
ATPrepDropExpression(Relation rel, AlterTableCmd *cmd, bool recurse, bool recursing, LOCKMODE lockmode)
{
	/*
	 * 如果有子表则拒绝 ONLY。我们可以实现它，但有点      
	 * 复杂。GENERATED 子句必须附加在列定义上，并且          
	 * 无法像 DEFAULT 那样以后添加，所以如果子表             
	 * 有一个父表没有的生成表达式，那么子表列必        
	 * 然是 attislocal 列。所以要在这里实现 ONLY，              
	 * 我们需要额外的代码来更新直接子表的                
	 * attislocal，有点类似于 DROP COLUMN 的做法，           
	 * 这样结果状态才能正确转储和恢复。    
	 */
	if (!recurse &&
		find_inheritance_children(RelationGetRelid(rel), lockmode))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / DROP EXPRESSION must be applied to child tables too")));

	/*
	 * 无法从继承列中删除生成表达式。            
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
 * 返回受影响列的地址。            
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
	 * 将该列标记为不再是生成列。（atthasdef 标志           
	 * 也要清除，但 RemoveAttrDefault 会处理它。）  
	 */
	attTup->attgenerated = '\0';
	CatalogTupleUpdate(attrelation, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);
	heap_freetuple(tuple);

	table_close(attrelation, RowExclusiveLock);

	/*
	 * 删除 GENERATED 表达式的依赖记录，特别是它对           
	 * 该列的 INTERNAL 依赖，否则 dependency.c 会               
	 * 拒绝执行删除操作。                    
	 */
	attrdefoid = GetAttrDefaultOid(RelationGetRelid(rel), attnum);
	if (!OidIsValid(attrdefoid))
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 RelationGetRelid(rel), attnum);
	(void) deleteDependencyRecordsFor(AttrDefaultRelationId, attrdefoid, false);

	/*  使上述变更可见     */
	CommandCounterIncrement();

	/*
	 * 删除 GENERATED 表达式本身。这里我们用 RESTRICT          
	 * 以求安全，但目前我们不期望有任何东西依赖     
	 * 它。  
	 */
	RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT,
					  false, false);

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}

/*
 * 更改表更改列设置统计目标   
 *
 * 返回值是被修改列的地址。              
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
	 * 我们允许仅对索引用数字引用列，因为表列的         
	 * 编号在以后删除列时可能出现空洞。               
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
	 * 限制目标到合理范围 
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
 * 返回值是被修改列的地址。              
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

	/*  生成新的 attoptions（文本数组）    */
	datum = SysCacheGetAttr(ATTNAME, tuple, Anum_pg_attribute_attoptions,
							&isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 castNode(List, options), NULL, NULL,
									 false, isReset);
	/*  校验新选项     */
	(void) attribute_reloptions(newOptions, true);

	/*  构建新元组 */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_attribute_attoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_attribute_attoptions - 1] = true;
	repl_repl[Anum_pg_attribute_attoptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(attrelation),
								 repl_val, repl_null, repl_repl);

	/*  更新系统表。    */
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
 * ATExecSetStorage 和 ATExecSetCompression 的                
 *
 * 辅助函数：为与指定表列关联的索引列设置        
 * attstorage/attcompression 字段。        
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
 * 更改表更改列设置存储策略
 *
 * 返回值是被修改列的地址。              
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
	 * 同时把变更应用到索引（仅适用于简单索引          
	 * 列，与 index.c ConstructTupleDescriptor() 一致）   
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
 * 更改表删除列     
 *
 * DROP COLUMN 无法使用普通的 ALTER TABLE 递归              
 * 机制，因为我们必须在运行时决定是否递归，            
 * 这取决于 attinhcount 是否会变为零。（我们               
 * 无法在静态预检查中确认，因为它无法正确处理多继  
 * 承。）  
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
 * 从关系 'rel' 中删除列 'colName' 并返回被删除列的          
 * 地址。该列也会被删除（或被标记为不再从          
 * 关系继承），如果有的话，也会从该关系的继承           
 *
 * 在针对继承子关系的递归调用中，不是直接               
 * 删除该列（如果要删除的话），而是把它的对象           
 * 地址加入 'addrs'，在这类调用中它必须为非             
 * NULL。所有列会在所有子表都被递归检查之后           
 * 同时删除。     
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

	/*  顶层权限检查已在 ATPrepCmd 完成，否则在此做     */
	if (recursing)
		ATSimplePermissions(AT_DropColumn, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*  在第一次调用时初始化 addrs    */
	Assert(!recursing || addrs != NULL);

	/*  由于此函数会递归，它可能被驱动到栈溢出         */
	check_stack_depth();

	if (!recursing)
		addrs = new_object_addresses();

	/*
	 * 取得该属性的编号       
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

	/*  无法删除系统属性     */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot drop system column \"%s\"",
						colName)));

	/*
	 * 不要删除继承的列，除非正在递归（大概是由          
	 * 于删除父列）   
	 */
	if (targetatt->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop inherited column \"%s\"",
						colName)));

	/*
	 * 也不要删除分区键中使用的列。（如果我们             
	 * 放行它，键列的依赖会导致整个表被级联删除，      
	 * 这肯定不是用户所期望的。）                         
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
	 * 酌情向子表传播。与大多数其他 ALTER 例程       
	 * 不同，我们必须一次做一层递归；我们无法              
	 * 用 find_all_inheritors 一次完成。      
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	if (children)
	{
		Relation	attr_rel;
		ListCell   *child;

		/*
		 * 对于分区表，该列也必须从                               
		 * 分区中删除。 
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

			/*  find_inheritance_children 已获得锁    */
			childrel = table_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			tuple = SearchSysCacheCopyAttName(childrelid, colName);
			if (!HeapTupleIsValid(tuple))	/*  不应发生    */
				elog(ERROR, "cache lookup failed for attribute \"%s\" of relation %u",
					 colName, childrelid);
			childatt = (Form_pg_attribute) GETSTRUCT(tuple);

			if (childatt->attinhcount <= 0) /*  不应发生    */
				elog(ERROR, "relation %u has non-inherited attribute \"%s\"",
					 childrelid, colName);

			if (recurse)
			{
				/*
				 * 如果子列还有其他定义来源，只需将其   
				 * 继承计数减一；如果没有，则递归删除它。 
				 *    
				 */
				if (childatt->attinhcount == 1 && !childatt->attislocal)
				{
					/*  现在也该删除这个子列了    */
					ATExecDropColumn(wqueue, childrel, colName,
									 behavior, true, true,
									 false, lockmode, addrs);
				}
				else
				{
					/*  子列必须在我的删除中存活 */
					childatt->attinhcount--;

					CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

					/*  使更新可见    */
					CommandCounterIncrement();
				}
			}
			else
			{
				/*
				 * 若被告知只在本表删除（不递归），          
				 * 则需把继承者属性标记为本地定义        
				 * 而不是继承的。         
				 */
				childatt->attinhcount--;
				childatt->attislocal = true;

				CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

				/*  使更新可见    */
				CommandCounterIncrement();
			}

			heap_freetuple(tuple);

			table_close(childrel, NoLock);
		}
		table_close(attr_rel, RowExclusiveLock);
	}

	/*  添加待删对象  */
	object.classId = RelationRelationId;
	object.objectId = RelationGetRelid(rel);
	object.objectSubId = attnum;
	add_exact_object_address(&object, addrs);

	if (!recursing)
	{
		/*  递归已结束，删除所有已收集的对象       */
		performMultipleDeletions(addrs, behavior, 0);
		free_object_addresses(addrs);
	}

	return object;
}

/*
 * 更改表加索引   
 *
 * 语法中没有这样的命令，但 parse_utilcmd.c 会             
 * 把 UNIQUE 和 PRIMARY KEY 约束转换成 AT_AddIndex 子命令。        
 * 这让我们能在 ALTER 期间的适当时刻安排创建索引。    
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

	/*  IndexStmt 已经经过 transformIndexStmt                */
	Assert(stmt->transformed);

	/*  重建现有索引时跳过模式权限检查              */
	check_rights = !is_rebuild;
	/*  若 Phase 3 将构建或我们在复用旧索引，则跳过构建*/
	skip_build = tab->rewrite > 0 || RelFileNumberIsValid(stmt->oldNumber);
	/*  重建现有索引时不发出通知           */
	quiet = is_rebuild;

	address = DefineIndex(RelationGetRelid(rel),
						  stmt,
						  InvalidOid,	/*  无预定义 OID */
						  InvalidOid,	/*  无父索引   */
						  InvalidOid,	/*  无父约束        */
						  -1,	/*  total_parts 未知 */
						  true, /*  is_alter_table*/
						  check_rights,
						  false,	/*  check_not_in_use - 已做过        */
						  skip_build,
						  quiet);

	/*
	 * 如果 TryReuseIndex() 为我们暂存了 relfilenumber，           
	 * 我们就会把它用于新索引，而不是从零开始构建。     
	 * 恢复关联的字段。这可能在两个字段中都存入        
	 * InvalidSubTransactionId，这种情况下 relcache.c                
	 * 会假定它可以重建 relcache 条目。因此，要在使         
	 * 系统表行对任何重建可见的 CCI 之后执行。索引旧版 
	 * 的 DROP 已安排提交时删除存储，故取消。  
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
 * 更改表添加统计     
 *
 * 语法中没有这样的命令，但我们在内部使用它来          
 * 添加 AT_ReAddStatistics 子命令，在表列类型变更后             
 * 重建统计。    
 */
static ObjectAddress
ATExecAddStatistics(AlteredTableInfo *tab, Relation rel,
					CreateStatsStmt *stmt, bool is_rebuild, LOCKMODE lockmode)
{
	ObjectAddress address;

	Assert(IsA(stmt, CreateStatsStmt));

	/* CreateStatsStmt 已经过 transformStatsStmt 处理             */
	Assert(stmt->transformed);

	address = CreateStatistics(stmt, !is_rebuild);

	return address;
}

/*
 * 更改表添加约束 USING INDEX     
 *
 * 返回新约束的地址。               
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
	 * 在分区表上实现这一功能并非易事，                      
	 * 暂且搁置。       
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ALTER TABLE / ADD CONSTRAINT USING INDEX is not supported on partitioned tables")));

	indexRel = index_open(index_oid, AccessShareLock);

	indexName = pstrdup(RelationGetRelationName(indexRel));

	indexInfo = BuildIndexInfo(indexRel);

	/* 本应在语法分析阶段就得到检查  */
	if (!indexInfo->ii_Unique)
		elog(ERROR, "index \"%s\" is not unique", indexName);

	/*
	 * 确定要赋予约束的名称。我们要求约束                
	 * 与底层索引同名；因此，默认使用索引的                
	 * 现有名称作为默认约束名；如果用户显式指定 
	 * 了其他名称，则重命名该索引以保持一致。            
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

	/* 若创建主键则需要额外的检查   */
	if (stmt->primary)
		index_check_primary_key(rel, indexInfo, true, stmt);

	/* 注意我们目前在这里不支持 EXCLUSION 约束      */
	if (stmt->primary)
		constraintType = CONSTRAINT_PRIMARY;
	else
		constraintType = CONSTRAINT_UNIQUE;

	/* 为该约束创建系统表条目             */
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
									  false);	/* is_internal */

	index_close(indexRel, NoLock);

	return address;
}

/*
 * 更改表添加约束     
 *
 * 返回值是新约束的地址；如果没有添加任何约束，     
 * 则返回 InvalidObjectAddress。       
 */
static ObjectAddress
ATExecAddConstraint(List **wqueue, AlteredTableInfo *tab, Relation rel,
					Constraint *newConstraint, bool recurse, bool is_readd,
					LOCKMODE lockmode)
{
	ObjectAddress address = InvalidObjectAddress;

	Assert(IsA(newConstraint, Constraint));

	/*
	 * 目前我们只期望 CONSTR_CHECK 和 CONSTR_FOREIGN 节点          
	 * 到达这里（参见 parse_utilcmd.c 中做的预处理）。       
	 * 仍用 switch，让日后添加代码更容易。      
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
			 * 分配或验证约束名称       
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
 * 根据引用被引用表的那些列名的列表，为新外键          
 * 生成约束名称中的列名部分。该名称将连同       
 * 父表名和 "fkey" 后缀一起传递给                                
 * ChooseConstraintName。          
 *
 * 我们知道实际使用的字符不会超过 NAMEDATALEN 个，因此    
 * 生成这么多字符之后即可截断结果。       
 *
 * XXX 另见 ChooseExtendedStatisticNameAddition      
 * ChooseIndexNameAddition.
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
			buf[buflen++] = '_';	/* 在名称间插入 _   */

		/*
		 * 此时我们有 buflen <= NAMEDATALEN。name 应该已经        
		 * 小于 NAMEDATALEN，但谨慎起见用 strlcpy。     
		 */
		strlcpy(buf + buflen, name, NAMEDATALEN);
		buflen += strlen(buf + buflen);
		if (buflen >= NAMEDATALEN)
			break;
	}
	return pstrdup(buf);
}

/*
 * 向单个表及其子表添加一个检查约束。返回添加到     
 * 父关系上的约束的地址（若未添加则                          
 * InvalidObjectAddress。）        
 *
 * ATExecAddConstraint 的子例程   
 *
 * 我们必须在执行期间递归到子表，而不是使用       
 * ALTER TABLE 通常的前期递归。原因在于所有约束         
 * 都*必须*被赋予相同的名称，否则以后它们不会       
 * 被视为相互关联。如果用户没有显式指定名称，   
 * 那么 AddRelationNewConstraints 通常会给子约束                
 * 分配不同的名称。为解决这个问题，我们必须         
 * 把父表处的名称传下去。   
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

	/* 顶层权限检查已在 ATPrepCmd 做过，否则在此做      */
	if (recursing)
		ATSimplePermissions(AT_AddConstraint, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*
	 * 调用 AddRelationNewConstraints 来完成工作，确保它          
	 * 处理 Constraint 的副本，使 transformExpr 无法修改原件     
	 * 它返回成熟约束的列表。    
	 *
	 * 如果该约束最终与已存在的约束合并，它将不会       
	 * 出现在返回的列表中。这正是我们想要的：无需      
	 * 做任何验证工作。这只可能发生在子表上，因为  
	 * 我们在顶层不允许合并。                  
	 */
	newcons = AddRelationNewConstraints(rel, NIL,
										list_make1(copyObject(constr)),
										recursing || is_readd,	/* allow_merge */
										!recursing, /* is_local */
										is_readd,	/* is_internal */
										NULL);	/* queryString not available
												 * here */

	/* 我们预期这里最多只有一个约束    */
	Assert(list_length(newcons) <= 1);

	/* 将每个待验证的约束加入第 3 阶段队列    */
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

		/* 若名称为默认，则保存实际指派名。    */
		if (constr->conname == NULL)
			constr->conname = ccon->name;

		ObjectAddressSet(address, ConstraintRelationId, ccon->conoid);
	}

	/* 此时必须已有一个可供使用的确定名称  */
	Assert(constr->conname != NULL);

	/* 推进命令计数器，以防同一个表被访问多次            */
	CommandCounterIncrement();

	/*
	 * 如果该约束已与某个现有约束合并，那么就完成      
	 * 了。此时不允许递归到子表，它们已有该约束， 
	 * 再访问它们会导致 coninhcount 的                            
	 * 值不正确。                 
	 */
	if (newcons == NIL)
		return address;

	/*
	 * 若添加 NO INHERIT 约束，无需查找子表。             
	 */
	if (constr->is_no_inherit)
		return address;

	/*
	 * 按需传播到子表。与其他多数 ALTER 例程不同， 
	 * 我们必须一次只做这一层递归；无法使用                 
	 * find_all_inheritors 一次性完成。       
	 */
	children =
		find_inheritance_children(RelationGetRelid(rel), lockmode);

	/*
	 * 检查 ALTER TABLE 是否指定了 ONLY。如果是，则      
	 * 仅在没有子表的情况下允许创建该约束；否则报        
	 * 错。    
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

		/* find_inheritance_children 已拿到锁     */
		childrel = table_open(childrelid, NoLock);
		CheckAlterTableIsSafe(childrel);

		/* 为该表查找或创建工作队列条目     */
		childtab = ATGetQueueEntry(wqueue, childrel);

		/* 递归到子表  */
		ATAddCheckConstraint(wqueue, childtab, childrel,
							 constr, recurse, true, is_readd, lockmode);

		table_close(childrel, NoLock);
	}

	return address;
}

/*
 * 向单个表添加外键约束；返回新约束的地址。               
 *         
 *
 * ATExecAddConstraint 的子例程。必须已持有该           
 * rel 上的排他锁，并且已对它做过适当的              
 * 不过权限检查在此进行。      
 *
 * 当被引用方或引用方（或两者）是分区表时，        
 * 需要多条 pg_constraint 记录 -- 每个分区表以及                
 * 每一侧的每个分区各一条（幸好并非每一种组               
 * 合都要一条）。在被引用侧的每个叶分区上要         
 * 有动作触发器，引用侧叶分区要有检查触发器。  
 *                  
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
	 * 在 pk 表上获取 ShareRowExclusiveLock，以免有人           
	 * 趁乱删走数据行。      
	 */
	if (OidIsValid(fkconstraint->old_pktable_oid))
		pkrel = table_open(fkconstraint->old_pktable_oid, ShareRowExclusiveLock);
	else
		pkrel = table_openrv(fkconstraint->pktable, ShareRowExclusiveLock);

	/*
	 * 有效性检查（权限检查要等到拿到列号之后）   
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
	 * 从永久表或非日志表引用临时表，以及从永久表      
	 * 引用非日志表也不被允许，因为被引用数据      
	 * 可能在我们不经意间消失。从临时表到任何其他    
	 * 表类型的引用同样不被允许，因为其他后端可能  
	 * 需要在永久表上运行 RI 触发器，但它们无法可靠      
	 * 地看到其他后端本地缓冲区中的元组。              
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
	 * 查找引用属性以确认它们存在，并记录它们的          
	 * attnum 和类型 OID。     
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
	 * 如果被引用表的属性列表被省略，则查找主键的       
	 * 定义并使用之。否则，验证提供的属性列表。无   
	 * 论哪种情况，都要找出索引 OID 和索引操作符类，   
	 * 以及各属性的 attnum 和类型 OID。                       
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
		/* 查找与列列表匹配的索引          */
		indexOid = transformFkeyCheckAttrs(pkrel, numpks, pkattnum,
										   opclasses);
	}

	/*
	 * 至此可检查权限。     
	 */
	checkFkeyPermissions(pkrel, pkattnum, numpks);

	/*
	 * 对生成列检查一些事项。       
	 */
	for (i = 0; i < numfks; i++)
	{
		char		attgenerated = TupleDescAttr(RelationGetDescr(rel), fkattnum[i] - 1)->attgenerated;

		if (attgenerated)
		{
			/*
			 * 按照 SQL 标准检查对 UPDATE/DELETE 动作的限制     
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
	 * 查找约束中要使用的相等操作符。           
	 *
	 * 注意我们必须小心区分实际的 PK 列类型与                
	 * 操作符类的声明输入类型，后者可能只与前者       
	 * 二进制兼容。声明的 opcintype 才是探测                  
	 * pg_amop 的正确依据。  
	 */
	if (numfks != numpks)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foreign key disagree")));

	/*
	 * 凭借先前已有的约束，我们也许可以避免扫描     
	 * 表来验证这一个。见下文。    
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

		/* 我们需要 pg_opclass 条目中的几项信息   */
		cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclasses[i]));
		if (!HeapTupleIsValid(cla_ht))
			elog(ERROR, "cache lookup failed for opclass %u", opclasses[i]);
		cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
		amid = cla_tup->opcmethod;
		opfamily = cla_tup->opcfamily;
		opcintype = cla_tup->opcintype;
		ReleaseSysCache(cla_ht);

		/*
		 * 检查它是否为 btree；目前它绝不会失败，因为    
		 * 没有其他索引访问方法支持唯一索引。如果将来   
		 * 出现了其他类型的唯一索引，我们就需要办法  
		 * 来判定哪个操作符策略号是相等。（要求所有  
		 * 此类索引 AM 都用 btree 的号合理吗？）   
		 */
		if (amid != BTREE_AM_OID)
			elog(ERROR, "only b-tree indexes are supported for foreign keys");
		eqstrategy = BTEqualStrategyNumber;

		/*
		 * 该索引最好有一个主相等操作符。                 
		 * 我们将用它作 PK = PK 比较。 
		 */
		ppeqop = get_opfamily_member(opfamily, opcintype, opcintype,
									 eqstrategy);

		if (!OidIsValid(ppeqop))
			elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
				 eqstrategy, opcintype, opcintype, opfamily);

		/*
		 * 有没有恰好接受 FK 类型的相等操作符？假设        
		 * 应假设要透过任何域查看。   
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
			/* 使编译器安静  */
			pfeqop_right = InvalidOid;
			ffeqop = InvalidOid;
		}

		if (!(OidIsValid(pfeqop) && OidIsValid(ffeqop)))
		{
			/*
			 * 否则，查找从 FK 类型到 opcintype 的隐式         
			 * 转换；若找到，就用主相等操作符。这有点  
			 * 棘手，因为 opcintype 可能是多态类型，例如     
			 * ANYARRAY 或 ANYENUM；所以我们要测试的是          
			 * 两个实际列类型能否同时转换到该类型。         
			 * （否则，我们将无法拒绝诸如 int[] 与            
			 * point[]。）         
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
			 * 当 pfeqop 变化时，重新验证该约束。我们      
			 * 本可允许操作符族内部的变化，但这带来微妙  
			 * 复杂性，对核心类型无实际益处。无需      
			 * 评估 ppeqop 与 ffeqop，RI_Initial_Check() 不用它们。  
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
			 * 确定从旧、新两侧 FK 列类型到 pfeqop 的右         
			 * （外）操作数类型的转换路径。我们可以假定   
			 * pg_constraint.conkey 不会变化。                    
			 */
			old_fktype = attr->atttypid;
			new_fktype = fktype;
			old_pathtype = findFkeyCast(pfeqop_right, old_fktype,
										&old_castfunc);
			new_pathtype = findFkeyCast(pfeqop_right, new_fktype,
										&new_castfunc);

			/*
			 * 当 FK 列到其 pfeqop 操作数的转换发生变       
			 * 化时，重新验证该约束。就此次评估而言，  
			 * 二进制强转等同无转换。虽然类型实现者      
			 * 设计隐式转换时会顾及相等等操作的一致    
			 * 性，但这里无法假定他们已经这样做了。   
			 *                                    
			 *
			 * 带有多态参数的函数可能随 get_fn_expr_argtype()  
			 * 的回应而任意改变行为。因此，当转换目标为 
			 * 多态时，只有输入类型完全未变我们才    
			 * 能跳过重新验证。仅就核心数据类型与操作    
			 * 类而言，这一要求不妨碍任何原本可能的优      
			 * 化。                             
			 *
			 * 如果该转换从基类型转换到其上的域，那么该   
			 * 域类型必定就是该唯一索引的 opcintype。       
			 * 如此一来主键列必然是域类型。由于该约束先  
			 * 前是有效的，外侧的所有值必然存在于主侧并   
			 * 且符合该域。所以无需在此特殊对待域。       
			 *                                                             
			 *                        
			 *
			 * 由于我们要求所有排序规则对相等有相同的    
			 * 概念（事实如此，因为 texteq 归结为按位相  
			 * 等），故此处不比较排序规则。 
			 *
			 * 我们无需直接考虑 PK 类型。它必然可二进制  
			 * 转换到唯一索引列的 opcintype，而                  
			 * ri_triggers.c 只会按该 opcintype 来处理             
			 * PK 值。改变 opcintype 也会改变 pfeqop。            
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

	/* 首先创建约束的系统表条目本身。      */
	address = addFkConstraint(addFkBothSides,
							  fkconstraint->conname, fkconstraint, rel, pkrel,
							  indexOid,
							  InvalidOid,	/* 无父约束         */
							  numfks,
							  pkattnum,
							  fkattnum,
							  pfeqoperators,
							  ppeqoperators,
							  ffeqoperators,
							  numfkdelsetcols,
							  fkdelsetcols,
							  false);

	/* 接着在被引用侧处理动作触发器并递归                 */
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

	/* 最后在引用侧创建检查触发器并递归                     */
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
	 * 完成。关闭 pk 表，但保持锁直到提交。       
	 */
	table_close(pkrel, NoLock);

	return address;
}

/*
 * validateFkOnDeleteSetColumns
 *		验证 ON DELETE SET NULL/DEFAULT (...) 中使用             
 *		的列是否有效。  
 *
 * 如果 fksetcolsattnums[] 数组中有重复项，此函数会        
 * 静默去除重复项。返回新的 numfksetcols 计数。  
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

		/* 确保它在 fkattnums[] 中  */
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

		/* 检查重复项    */
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
 * addFkConstraint
 *		安装 pg_constraint 条目来实现外键约束。                 
 *		调用者必须酌情另行调用                       
 *		addFkRecurseReferenced 和 addFkRecurseReferencing 以                
 *		安装 pg_trigger 条目；分区表则递归。   
 *
 * fkside: 待创建的 FK 侧（或两侧）。                 
 *      addFkReferencedSide 调用 addFkRecurseReferenced；       
 *      addFkReferencingSide 调用 addFkRecurseReferencing；               
 *      addFkBothSides 
 * constraintname: 所添加约束的基准名称，若         
 *      fkconstraint->conname 未设置则复制给它          
 * fkconstraint: 所添加的约束        
 * rel: 根引用关系              
 * pkrel: 被引用关系；递归时可能是分区                 
 * indexOid: 在 pkrel 上实现该约束的索引 OID                   
 * parentConstr: 父约束的 OID；顶层约束时为                  
 *      InvalidOid。       
 * numfks: 外键中的列数                      
 * pkattnum: 被引用属性的 attnum 数组         
 * fkattnum: 引用属性的 attnum 数组             
 * pf/pp/ffeqoperators: 列间操作符的 OID 数组         
 * numfkdelsetcols: (...) ON DELETE SET NULL/DEFAULT                       
 *      列数。   
 * fkdelsetcols: ON DELETE SET NULL/DEFAULT 子句                   
 *      的 attnum 数组  
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
	 * 验证每个被引用分区的 relkind。在顶层这一步          
	 * 与先前某项检查重复，但递归时需要它。            
	 */
	if (pkrel->rd_rel->relkind != RELKIND_RELATION &&
		pkrel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("referenced relation \"%s\" is not a table",
						RelationGetRelationName(pkrel))));

	/*
	 * 调用者为我们提供了约束名；但它可能已被此         
	 * 分区占用，若是如此则另取一个名称。          
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
		 * 分区表一律继承，传统继承则绝不继承                
		 */
		connoinherit = rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE;
	}

	/*
	 * 将外键约束记入 pg_constraint。    
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
									  InvalidOid,	/* 不是域约束         */
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
									  NULL, /* 没有排他约束      */
									  NULL, /* 没有检查约束  */
									  NULL,
									  conislocal,	/* islocal */
									  coninhcount,	/* inhcount */
									  connoinherit, /* conNoInherit */
									  is_internal); /* is_internal */

	ObjectAddressSet(address, ConstraintRelationId, constrOid);

	/*
	 * 在分区场景中，要为该约束创建依赖条目。（ 
	 * 区的情形，相关条目由 CreateConstraintEntry                  
	 * 创建。）              
	 *
	 * 在被引用侧，我们需要该约束对其父约束有内      
	 * 部依赖；这意味着该约束无法被单独删除 -- 只       
	 * 能通过父约束删除。这也意味着包含它的分区          
	 * 也无法被单独删除，但可以被分离，分离时该依      
	 * 赖会被移除（在验证没有行经此 FK 被引用之          
	 * 后。）                                          
	 *
	 * 在处理引用侧时，我们通过特殊的分区依赖来        
	 * 链接该约束：父约束是首要依赖者，而外键所           
	 * 在的分区是次要依赖。这样，任一对象被删除       
	 * 时该约束也会被删除。                                       
	 *                     
	 *
	 * 注意只有分区中的附属 pg_constraint 行才需要             
	 * 这些；最顶层的行不需要其中任何一项。  
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

	/* 使新约束可见，以备添加更多约束    */
	CommandCounterIncrement();

	return address;
}

/*
 * addFkRecurseReferenced
 *		外键创建中被引用侧的递归辅助函数，它创建     
 *		动作触发器并递归                      
 *
 * 如果被引用关系是普通关系，创建实现该约束               
 * 所需的动作触发器。如果被引用关系是分区表，         
 * 那么就为它创建一条引用引用侧父表的                         
 * pg_constraint 行，并针对每个分区对本例程递归。     
 *           
 *
 * fkconstraint: 所添加的约束        
 * rel: 根引用关系              
 * pkrel: 被引用关系；递归时可能是分区                 
 * indexOid: 在 pkrel 上实现该约束的索引 OID                   
 * parentConstr: 父约束的 OID；顶层约束时为                  
 *      InvalidOid。       
 * numfks: 外键中的列数                      
 * pkattnum: 被引用属性的 attnum 数组         
 * fkattnum: 引用属性的 attnum 数组             
 * numfkdelsetcols: ON DELETE SET NULL/DEFAULT 子句         
 *      的列数                
 * fkdelsetcols: ON DELETE SET NULL/DEFAULT 子句                   
 *      的 attnum 数组  
 * pf/pp/ffeqoperators: 列间操作符的 OID 数组         
 * old_check_ok: 若该约束替换了某个已验证过的约        
 *      束（因此这个无需验证），则为 true。           
 * parentDelTrigger 和 parentUpdTrigger：在分区上               
 *      递归调用时，分别是 DELETE 与 UPDATE 父动作         
 *      触发器的 OID。 
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
	 * 创建实施该约束的动作触发器。             
	 */
	createForeignKeyActionTriggers(rel, RelationGetRelid(pkrel),
								   fkconstraint,
								   parentConstr, indexOid,
								   parentDelTrigger, parentUpdTrigger,
								   &deleteTriggerOid, &updateTriggerOid);

	/*
	 * 如果被引用表是分区表，则对自身递归以处理每       
	 * 个分区。除了父表的 pg_constraint 行之外，还需   
	 * 为每个分区创建一条 pg_constraint 行。                    
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

			/* XXX 提前获取这些锁会不会更好？               */
			partRel = table_open(pd->oids[i], ShareRowExclusiveLock);

			/*
			 * 将 FK 定义中被引用侧的属性号映射，以匹   
			 * 配分区的列布局。                          
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

			/* 确定此层级要使用的索引        */
			partIndexId = index_get_partition(partRel, indexOid);
			if (!OidIsValid(partIndexId))
				elog(ERROR, "index for %u not found in partition %s",
					 indexOid, RelationGetRelationName(partRel));

			/* 在此层级创建条目...    */
			address = addFkConstraint(addFkReferencedSide,
									  fkconstraint->conname, fkconstraint, rel,
									  partRel, partIndexId, parentConstr,
									  numfks, mapped_pkattnum,
									  fkattnum, pfeqoperators, ppeqoperators,
									  ffeqoperators, numfkdelsetcols,
									  fkdelsetcols, true);
			/* ...并且递归到子表        */
			addFkRecurseReferenced(fkconstraint, rel, partRel,
								   partIndexId, address.objectId, numfks,
								   mapped_pkattnum, fkattnum,
								   pfeqoperators, ppeqoperators, ffeqoperators,
								   numfkdelsetcols, fkdelsetcols,
								   old_check_ok,
								   deleteTriggerOid, updateTriggerOid);

			/* 完成 -- 清理（但保留锁）   */
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
 *		外键创建中引用侧的递归辅助函数，它创建检      
 *		查触发器并递归                        
 *
 * 如果引用关系是普通关系，创建实现该约束所               
 * 需的检查触发器，并为第 3 阶段的约束验证做好          
 * 准备。如果引用关系是分区表，则为它创建一条        
 * pg_constraint 行，并针对每个分区对本例程递归。           
 *           
 *
 * 我们假定被引用关系已被锁定而不受并发删除       
 * 影响。如果它是分区关系，则每个分区都须             
 * 如此 
 *
 * wqueue: ALTER TABLE 工作队列；若不在 ALTER               
 *       TABLE 序列为 NULL。   
 * fkconstraint: 所添加的约束        
 * rel: 引用关系；递归时可能是分区                     
 * pkrel: 根被引用关系          
 * indexOid: 在 pkrel 上实现该约束的索引 OID                   
 * parentConstr: 父约束的 OID（总有一个）                    
 * numfks: 外键中的列数                      
 * pkattnum: 被引用属性的 attnum 数组         
 * fkattnum: 引用属性的 attnum 数组             
 * pf/pp/ffeqoperators: 列间操作符的 OID 数组         
 * numfkdelsetcols: ON DELETE SET NULL/DEFAULT 子句                      
 *      的列数   
 * fkdelsetcols: ON DELETE SET NULL/DEFAULT 子句                   
 *      的 attnum 数组  
 * old_check_ok: 若该约束替换了某个已验证过的约        
 *      束（因此这个无需验证），则为 true。           
 * lockmode: 递归到分区时要获取的锁模式             
 * parentInsTrigger 和 parentUpdTrigger：在分区上被                
 *      递归调用时，分别是 INSERT 与 UPDATE 父检             
 *      查触发器 OID。 
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
	 * 向它添加检查触发器，必要时安排它在第           
	 * 3 阶段检查。  
	 *
	 * 如果该关系是分区表，则深入到其分区同样处理。    
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
		 * 告诉第 3 阶段检查现有行是否满足该约束。在      
		 * 创建表期间可以跳过这一步；在 ADD FOREIGN      
		 * KEY 命令中显式指定 NOT VALID 时也可以跳过；        
		 * 以及在 SET DATA TYPE 操作未损及其有效性的          
		 * 重建约束之后同样如此。          
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
		 * 外键的触发器将在下面的循环中被反复操作。     
		 * 为避免反复打开/关闭触发器系统表，我们在         
		 * 这里将其打开，并传递给下面调用的各子例程。 
		 *              
		 */
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

		/*
		 * 对每个分区递归以采取相应动作；要么找到一   
		 * 个可改挂到我们名下的现有约束，或新建一个       
		 * 。 
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

			/* 检查是否可以将现有约束改作他用          */
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
			 * 没找到可复用的合适约束；就自建一个。     
			 */
			address = addFkConstraint(addFkReferencingSide,
									  fkconstraint->conname, fkconstraint,
									  partition, pkrel, indexOid, parentConstr,
									  numfks, pkattnum,
									  mapped_fkattnum, pfeqoperators,
									  ppeqoperators, ffeqoperators,
									  numfkdelsetcols, fkdelsetcols, true);

			/* 调用自身以完成创建，然后就大功告成    */
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
 *		把外键从分区表克隆到新获得的分区。            
 *		          
 *
 * partitionRel 是 parentRel 的一个分区，因此可以肯              
 * 定其各列与父表相同且类型一致。但顺序                    
 * 不同。     
 *
 * wqueue 必须传入以设置第 3 阶段约束检查，除非已       
 * 知引用侧分区为空（如 CREATE TABLE / PARTITION OF                
 * 时）。     
 */
static void
CloneForeignKeyConstraints(List **wqueue, Relation parentRel,
						   Relation partitionRel)
{
	/* 这只适用于声明式分区               */
	Assert(parentRel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/*
	 * 首先，克隆父表位于引用侧的那些约束。               
	 */
	CloneFkReferencing(wqueue, parentRel, partitionRel);

	/*
	 * 克隆父表位于被引用侧的那些约束。                 
	 */
	CloneFkReferenced(parentRel, partitionRel);
}

/*
 * CloneFkReferenced
 *		CloneForeignKeyConstraints 的子例程  
 *
 * 找出所有父关系位于被引用侧的外键；把这些          
 * 约束克隆到给定的分区。应在创建或附加分区         
 * 时调用本函数。                           
 *
 * 如果要附加的关系是分区表，则本函数会递归               
 * 到分区，经 addFkRecurseReferenced 递归。    
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
	 * 搜索父关系位于被引用侧的所有约束。但是，      
	 * 我们不允许克隆那些父约束也将被克隆的约束，  
	 * 以免重复。所以分两步走：先构建待克隆约束          
	 * 的列表，然后遍历该列表，克隆那些父约束不         
	 * 在列表中的约束。（我们不允许依赖先看到父约        
	 * 束，因为系统表扫描可能先返回子约束。）          
	 *                                   
	 */
	pg_constraint = table_open(ConstraintRelationId, RowShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(parentRel)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));
	/* 这是顺序扫描，因为没有可用的索引...    */
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
	 * 外键的触发器将在下面的循环中被反复操作。        
	 * 为避免反复打开/关闭触发器系统表，我们在      
	 * 这里将其打开，并传递给下面调用的各子例程。        
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
		 * 如上文所释：不要试图克隆那些父约束将被我       
		 * 们克隆的约束。     
		 */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/* 我们需要 CreateTrigger 将获取的那一级锁         */
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
		/* ->fk_attrs 稍后确定     */
		fkconstraint->pk_attrs = NIL;
		fkconstraint->fk_matchtype = constrForm->confmatchtype;
		fkconstraint->fk_upd_action = constrForm->confupdtype;
		fkconstraint->fk_del_action = constrForm->confdeltype;
		fkconstraint->fk_del_set_cols = NIL;
		fkconstraint->old_conpfeqop = NIL;
		fkconstraint->old_pktable_oid = InvalidOid;
		fkconstraint->skip_validation = false;
		fkconstraint->initially_valid = true;

		/* 设置用于生成约束名称的列名                       */
		for (int i = 0; i < numfks; i++)
		{
			Form_pg_attribute att;

			att = TupleDescAttr(RelationGetDescr(fkRel),
								conkey[i] - 1);
			fkconstraint->fk_attrs = lappend(fkconstraint->fk_attrs,
											 makeString(NameStr(att->attname)));
		}

		/*
		 * 添加指向新分区的新外键约束。由于这个新分     
		 * 区出现在约束的被引用侧，无需为第 3 阶段       
		 * 检查做设置。                                    
		 */
		partIndexId = index_get_partition(partitionRel, indexOid);
		if (!OidIsValid(partIndexId))
			elog(ERROR, "index for %u not found in partition %s",
				 indexOid, RelationGetRelationName(partitionRel));

		/*
		 * 取出属于该约束的 "action" 触发器，用作即将      
		 * 在分区的 addFkRecurseReferenced() 中创建的类       
		 * 似触发器的父 OID。             
		 */
		GetForeignKeyActionTriggers(trigrel, constrOid,
									constrForm->confrelid, constrForm->conrelid,
									&deleteTriggerOid, &updateTriggerOid);

		/* 添加该约束...      */
		address = addFkConstraint(addFkReferencedSide,
								  fkconstraint->conname, fkconstraint, fkRel,
								  partitionRel, partIndexId, constrOid,
								  numfks, mapped_confkey,
								  conkey, conpfeqop, conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols, false);
		/* ...并递归    */
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
 * 对于给定列表中父关系的每个外键约束，在其            
 * 分区关系中找出一个可被改挂的等价约束；如           
 * 果找不到，就在分区中创建一个新约束作为其           
 *       
 *
 * 如果给出 wqueue，则用它为每个被克隆的约束            
 * 设置第 3 阶段验证；不需要时可省略             
 * （例如：分区正在新建时）。        
 */
static void
CloneFkReferencing(List **wqueue, Relation parentRel, Relation partRel)
{
	AttrMap    *attmap;
	List	   *partFKs;
	List	   *clone = NIL;
	ListCell   *cell;
	Relation	trigrel;

	/* 取得我们需要克隆的约束的列表         */
	foreach(cell, RelationGetFKeyList(parentRel))
	{
		ForeignKeyCacheInfo *fk = lfirst(cell);

		/*
		 * 拒绝将某个本分区表已有外键指向的表附加为     
		 * 分区。这种模式毫无用处，从没有用户抱怨反     
		 * 方向做不到这一点这一事实即可证明；所谓反   
		 * 方向，即创建引用某个分区的外键。这一限制      
		 * 让我们得以绕开在 ATTACH/DETACH 期间为这类         
		 * 关系所需的 pg_constraint 与 pg_trigger 行的复 
		 * 杂之处。                                                   
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
	 * 如果无事可做就静默地什么都不做。特别是，      
	 * 这避免了对外表抛出虚假错误。          
	 */
	if (clone == NIL)
		return;

	if (partRel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("foreign key constraints are not supported on foreign tables")));

	/*
	 * 外键的触发器将在下面的循环中被反复操作。        
	 * 为避免反复打开/关闭触发器系统表，我们在      
	 * 这里将其打开，并传递给下面调用的各子例程。        
	 *       
	 */
	trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * 如果分区中的列不同，约束键可能不同。此映      
	 * 射用于转换它们。                     
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

		/* 不要克隆那些父约束正被克隆的约束       */
		if (list_member_oid(clone, constrForm->conparentid))
		{
			ReleaseSysCache(tuple);
			continue;
		}

		/*
		 * 必须防止并发删除。如果 pkrel 是分区关系，      
		 * 那就意味着要锁定所有分区。     
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
		 * 取出属于该约束的 "check" 触发器，用作即将在   
		 * 分区的 addFkRecurseReferencing() 中创建的类似      
		 * 触发器的父 OID。它们也被传递给下方的            
		 * tryAttachPartitionForeignKey()，以直接把它们指派          
		 * 为分区的现有 "check" 触发器充当父，即当    
		 * 相应约束被判定为可附加到父约束时。         
		 *            
		 */
		GetForeignKeyCheckTriggers(trigrel, constrForm->oid,
								   constrForm->confrelid, constrForm->conrelid,
								   &insertTriggerOid, &updateTriggerOid);

		/*
		 * 在创建新约束之前，先看看现有外键中是否有      
		 * 合适的。如果有，就把父约束附加到它上面，    
		 * 什么都不克隆。这样我们避免了昂贵的验证步  
		 * 骤，也不会得到重复的外键，而且无需为该约  
		 * 束递归到分区。                                   
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

		/* 未找到。着手创建自己的约束       */
		fkconstraint = makeNode(Constraint);
		fkconstraint->contype = CONSTRAINT_FOREIGN;
		/* ->conname 稍后确定     */
		fkconstraint->deferrable = constrForm->condeferrable;
		fkconstraint->initdeferred = constrForm->condeferred;
		fkconstraint->location = -1;
		fkconstraint->pktable = NULL;
		/* ->fk_attrs 稍后确定     */
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

		/* 在此层级创建 pg_constraint 条目      */
		address = addFkConstraint(addFkReferencingSide,
								  NameStr(constrForm->conname), fkconstraint,
								  partRel, pkrel, indexOid, parentConstrOid,
								  numfks, confkey,
								  mapped_conkey, conpfeqop,
								  conppeqop, conffeqop,
								  numfkdelsetcols, confdelsetcols,
								  false);

		/* 用完被克隆约束的元组          */
		ReleaseSysCache(tuple);

		/* 创建检查触发器，若有分区则递归到分区       */
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
								false,	/* 不存在旧检查  */
								AccessExclusiveLock,
								insertTriggerOid,
								updateTriggerOid);
		table_close(pkrel, NoLock);
	}

	table_close(trigrel, RowExclusiveLock);
}

/*
 * 当分区的父表收到外键的[引用侧]时，必须把                
 * 该外键传播到分区。然而，分区可能已经有一           
 * 个等价的外键；本例程把给定的                          
 * ForeignKeyCacheInfo（在分区中）与其余参数                       
 * 所定义的外键作比较。若二者等价，则创建两             
 * 约束间的链接并返回 true。 
 *
 * 如果给定的 FK 与其余参数定义的外键不匹配则返回  
 * false。     
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
	 * 做一些快速而简单的初步检查；任何一项失败         
	 * 即弃用。        
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
	 * 目前看来不错；再做一些更深入的检查。关于           
	 * 'convalidated' 的检查本可省去，因为我们并不真         
	 * 在意，但暂时小心为妙。  
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
	 * 很好！附加该约束。新分区中的动作触发器变        
	 * 得多余 -- 父表已经有等价的触发器，而且那些        
	 * 触发器能够到达该分区。删除分区中的那些。         
	 * 我们通过它们具有我们的约束 OID 且位于被引         
	 * 用关系上来识别它们。              
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
		 * 该约束最初被设置为把此触发器包含为实现对     
		 * 象，因此有一条依赖记录将两者链接起来；然    
		 * 而，由于不再需要该触发器，我们移除该依赖      
		 * 链接，以便能在保持约束完好无损的同时删除     
		 * 该触发器。               
		 */
		deleteDependencyRecordsFor(TriggerRelationId,
								   trgform->oid,
								   false);
		/* 使依赖删除对 performDeletion 可见           */
		CommandCounterIncrement();
		ObjectAddressSet(trigger, TriggerRelationId,
						 trgform->oid);
		performDeletion(&trigger, DROP_RESTRICT, 0);
		/* 使触发器删除可见，以防循环继续迭代  */
		CommandCounterIncrement();
	}

	systable_endscan(scan);

	ConstraintSetParentConstraint(fk->conoid, parentConstrOid, partRelid);

	/*
	 * 与约束一样，把分区的 "check" 触发器附加到对   
	 * 应的父触发器上。      
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
	 * 如果被引用表是分区表，那么现在附加的分区    
	 * 中会有些多余的 pg_constraint 行和动作触发器。           
	 * 把那些删除掉。           
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
				 * 先删除绑定各约束记录的那条依赖记录。   
				 *                             
				 */
				n = deleteDependencyRecordsForSpecific(ConstraintRelationId,
													   conform->oid,
													   DEPENDENCY_INTERNAL,
													   ConstraintRelationId,
													   fk->conoid);
				Assert(n == 1); /* 实际上预期只有一个   */

				/*
				 * 现在搜索该约束的触发器，也把它们设置 
				 * 为待删除。         
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
		/* 使这些依赖删除变为可见     */
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
 * GetForeignKeyActionTriggers
 * 		返回给定关系中属于给定约束的删除和更新        
 * 		"action" 触发器。            
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
		/* 只查看 PK 侧的 "action" 触发器。           */
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
		/* 在启用断言的构建中，继续查找以发现重复项    */
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
 * GetForeignKeyCheckTriggers
 * 		返回给定关系中属于给定约束的插入和更新       
 * 		"check" 触发器。             
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
		/* 只查看 FK 侧的 "check" 触发器。           */
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
		/* 在启用断言的构建中，继续查找以发现重复项。  */
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
 * 更改表更改约束       
 *
 * 更新约束的属性。              
 *
 * 目前仅适用于外键约束。                
 *
 * 若修改了该约束，则返回其地址；否则返回            
 * InvalidObjectAddress.
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
	 * 查找并检查目标约束         
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

	/* 最多只能有一个匹配的行     */
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
	 * 如果不是最顶层的约束，则报错。      
	 *
	 * 改动非最顶层的约束会使某些触发器不受影响，        
	 * 因为它们没有直接连接到该约束；此外，                 
	 * pg_dump 会忽略单个约束的可延迟状态，因为它           
	 * 只转储最顶层的约束。可以通过拒绝该操作并          
	 * 告知用户改为改动父约束来避免这些问题。             
	 */
	if (OidIsValid(currcon->conparentid))
	{
		HeapTuple	tp;
		Oid			parent = currcon->conparentid;
		char	   *ancestorname = NULL;
		char	   *ancestortable = NULL;

		/* 循环查找最顶层的约束      */
		while (HeapTupleIsValid(tp = SearchSysCache1(CONSTROID, ObjectIdGetDatum(parent))))
		{
			Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);

			/* 若无父约束，即为目标约束         */
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
	 * 执行实际的系统表操作。如果已处于目标状态，    
	 * 可以跳过修改；但分区表例外：无论如何都要        
	 * 处理分区，以防它们曾在本地改动过该约束。          
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
	 * ATExecAlterConstrRecurse 已让拥有该约束本身                   
	 * 的关系的 relcache 失效；这里我们还要让拥有         
	 * 属于该约束的触发器的关系失效化。       
	 */
	foreach(lc, otherrelids)
		CacheInvalidateRelcacheByRelid(lfirst_oid(lc));

	systable_endscan(scan);

	table_close(tgrel, RowExclusiveLock);
	table_close(conrel, RowExclusiveLock);

	return address;
}

/*
 * ATExecAlterConstraint 的递归子例程。若约束               
 * 改则返回 true。  
 *
 * *otherrelids 是包含受影响触发器的关系 OID 的               
 *
 * 注意即使值都正确我们也必须递归，以防间接       
 * 后代曾在本地改动过其约束。（如果禁止在分    
 * 区中改动约束本可避免这一点，但现有发行版           
 * 并没有这样做。）             
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

	/* 由于本函数会递归，它可能被驱动到栈溢出          */
	check_stack_depth();

	currcon = (Form_pg_constraint) GETSTRUCT(contuple);
	conoid = currcon->oid;
	refrelid = currcon->confrelid;

	/*
	 * 用 cmdcon 中的标志更新 pg_constraint。  
	 *
	 * 若被调用来修改一个已处于目标状态的约束，         
	 * 则默不作声。  
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

		/* 让新的约束标志对其他人可见     */
		CacheInvalidateRelcache(rel);

		/*
		 * 现在我们需要更新 pg_trigger 中实现该约束的    
		 * 那些多个条目。    
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
			 * 记住外键约束牵涉的其他关系（们）的 OID。   
			 * （注意：对于没有属性会变化的触发器所在的  
			 * 其他关系本可跳过 relcache 失效，但我们     
			 * 还是保守行事。）           
			 */
			if (tgform->tgrelid != RelationGetRelid(rel))
				*otherrelids = list_append_unique_oid(*otherrelids,
													  tgform->tgrelid);

			/*
			 * 更新 RI_FKey_noaction_del、               
			 * RI_FKey_noaction_upd、RI_FKey_check_ins 和                 
			 * RI_FKey_check_upd 的可延迟性；不含其他。         
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
	 * 如果约束任一端的表是分区表，我们需要递归           
	 * 并处理作为其子项的每个约束。                      
	 *
	 * （这假定递归标志对分区表被强制置位，而对       
	 * 传统继承则不置位，不过我们在此不检查这一         
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
 * 更改表验证约束          
 *
 * XXX 我们在 这里而非第 1 阶段处理递归，原因是           
 * 处理外键时没有办法跳过递归：那种情况下无              
 * 需锁定子表，但在那一层我们无法避免这样做。        
 *                        
 *
 * 返回值是已验证约束的地址。如果该约束已经               
 * 被验证过，则返回 InvalidObjectAddress。        
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
	 * 查找并检查目标约束         
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

	/* 最多只能有一个匹配的行     */
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

			/* 为第 3 阶段排队验证  */
			fkconstraint = makeNode(Constraint);
			/* 眼下只需这些          */
			fkconstraint->conname = constrName;

			newcon = (NewConstraint *) palloc0(sizeof(NewConstraint));
			newcon->name = constrName;
			newcon->contype = CONSTR_FOREIGN;
			newcon->refrelid = con->confrelid;
			newcon->refindid = con->conindid;
			newcon->conid = con->oid;
			newcon->qual = (Node *) fkconstraint;

			/* 为该表查找或创建工作队列条目     */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * 不允许对分区表创建无效外键，所以    
			 * 忽略递归位是安全的。                            
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
			 * 如果我们在递归，父级已经做过了，所以跳过 
			 * 它。此外，如果该约束是 NO INHERIT 约束，     
			 * 就不应尝试在子表中查找它。      
			 */
			if (!recursing && !con->connoinherit)
				children = find_all_inheritors(RelationGetRelid(rel),
											   lockmode, NULL);

			/*
			 * 对于 CHECK 约束，我们必须确保只有当子表上 
			 * 的约束已验证时，才在父表上把该约束标记为   
			 * 已验证。    
			 *
			 * 我们在父表验证前先递归，以降低死锁的风    
			 * 险。    
			 */
			foreach(child, children)
			{
				Oid			childoid = lfirst_oid(child);
				Relation	childrel;

				if (childoid == RelationGetRelid(rel))
					continue;

				/*
				 * 如果被告知不要递归，那么最好没有任何子 
				 * 表，因为除非约束对所有子表有效，否则   
				 * 无法把父表上的约束标记为有效。        
				 */
				if (!recurse)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("constraint must be validated on child tables too")));

				/* find_all_inheritors 已拿到锁     */
				childrel = table_open(childoid, NoLock);

				ATExecValidateConstraint(wqueue, childrel, constrName, false,
										 true, lockmode);
				table_close(childrel, NoLock);
			}

			/* 为第 3 阶段排队验证  */
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

			/* 为该表查找或创建工作队列条目     */
			tab = ATGetQueueEntry(wqueue, rel);
			tab->constraints = lappend(tab->constraints, newcon);

			/*
			 * 让 relcache 失效，使他人可见新验证的       
			 * 约束。  
			 */
			CacheInvalidateRelcache(rel);
		}

		/*
		 * 现在更新系统表，趁门还开着。          
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
		address = InvalidObjectAddress; /* 已验证         */

	systable_endscan(scan);

	table_close(conrel, RowExclusiveLock);

	return address;
}


/*
 * transformColumnNameList - 转换列名列表            
 *
 * 查找每个名称并返回其 attnum 及可选类型 OID。    
 *
 * 注意：此函数的名称表明它是通用的，但实际       
 * 上它只用于查找外键子句中出现的名称。错误         
 * 消息需要做些修改才能用于其他情形，有效性          
 * 检查也许同样如此。             
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
 *	查找 pkrel 的主键属性的名称、attnum 和类              
 *	型。同时返回支持主键的索引的 OID 和索引操         
 *	作符类。                     
 *
 *	除 pkrel 外的所有参数都是输出参数。此外，            
 *	函数返回值是主键中属性的数量。               
 *
 *	在 REFERENCES 规范中省略列清单时使用。                  
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
	 * 从 relcache 取得该表的索引 OID 列表，逐个在              
	 * pg_index 系统缓存中查找，直到找到标记为主键的那个 
	 * （但愿没有多于一个）。同时坚持它必须有效。     
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
			 * 拒绝使用可延迟主键。这是 SQL 规范的要求，   
			 * 而且如果尝试允许它，会有很多有趣的语义问   
			 * 题。            
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
	 * 确认已找到它    
	 */
	if (!OidIsValid(*indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no primary key for referenced table \"%s\"",
						RelationGetRelationName(pkrel))));

	/* indclass 需费力获取       */
	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	/*
	 * 现在根据 indkey 定义构建 PK 属性的列表（我         
	 * 们假定主键无法含有表达式元素）          
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
 *	验证 'pkrel' 关系中的 'attnums' 列作为外键约                
 *	束的一部分来引用是否有效。       
 *
 *	返回支持该约束的唯一索引的 OID，并使用与       
 *	索引列相关联的操作符类来填充调用者提供的      
 *	'opclasses' 数组。             
 *
 *	验证失败时引发 ERROR。        
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
	 * 拒绝所引用列清单中重复出现的列。SQL 标准             
	 * 禁止这种情形；即便我们认为允许它有用，把           
	 * 该清单与唯一索引相匹配也会有歧义（特别是       
	 * 不清楚哪个索引操作符类对应哪个 FK 列）。        
	 *                                    
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
	 * 从 relcache 取得该表的索引 OID 列表，在                    
	 * pg_index 系统缓存逐个查找，把唯一索引与                 
	 * attnum 清单匹配。  
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
		 * 列数必须正确；必须是唯一索引且不得是部分   
		 * 索引；如果含有任何表达式也要放弃。无效索       
		 * 引也不行。         
		 */
		if (indexStruct->indnkeyatts == numattrs &&
			indexStruct->indisunique &&
			indexStruct->indisvalid &&
			heap_attisnull(indexTuple, Anum_pg_index_indpred, NULL) &&
			heap_attisnull(indexTuple, Anum_pg_index_indexprs, NULL))
		{
			Datum		indclassDatum;
			oidvector  *indclass;

			/* indclass 需费力获取       */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, indexTuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/*
			 * 给定的 attnum 列表可以按任何顺序匹配索引       
			 * 列。检查是否匹配，并顺便提取适当的操作符  
			 * 类。      
			 *
			 * 我们知道 attnums[] 没有重复（依据本函数开    
			 * 头部的测试），且上面已检查列数相符，所以   
			 * 如果为每个 attnums[] 条目都找到了匹配，那么   
			 * 必然存在某种顺序的一对一匹配。            
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
			 * 拒绝使用可延迟的唯一/主键。这是 SQL 规范      
			 * 的要求，而且如果尝试允许它，会有很多有趣   
			 * 的语义问题。      
			 */
			if (found && !indexStruct->indimmediate)
			{
				/*
				 * 记住我们找到了一个其他方面匹配的索引，  
				 * 以便能生成更合适的错误消息。       
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
 *	find_coercion_pathway() 的包装器，供 ATAddForeignKeyConstraint   
 *	() 调用所用。二进制可转换与精确匹配同等看待。     
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
			/* 先前依赖的转换现在不见了。    */
			elog(ERROR, "could not find cast from %u to %u",
				 sourceTypeId, targetTypeId);
	}

	return ret;
}

/*
 * ADD FOREIGN KEY 时对被引用表的权限检查             
 *
 * 注意：我们已经检查过用户拥有引用表，否则           
 * 早就该失败退出了；对它无需做额外的检查。               
 */
static void
checkFkeyPermissions(Relation rel, int16 *attnums, int natts)
{
	Oid			roleid = GetUserId();
	AclResult	aclresult;
	int			i;

	/* 有关系级 REFERENCES 权限即可                 */
	aclresult = pg_class_aclcheck(RelationGetRelid(rel), roleid,
								  ACL_REFERENCES);
	if (aclresult == ACLCHECK_OK)
		return;
	/* 否则每列都必须有 REFERENCES 权限  */
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
 * 扫描表中现有的行，验证它们满足提议的 FK          
 * 约束。  
 *
 * 调用者必须已经适当地打开并锁定两个关系。    
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
	 * 构建触发器调用结构；反正都需要它。      
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
	/* 我们无需填写其余成员      */

	/*
	 * 看看能否用一个 LEFT JOIN 查询完成。若结果为       
	 * 假，则表明必须改用触发触发器的方法。     
	 */
	if (RI_Initial_Check(&trig, rel, pkrel))
		return;

	/*
	 * 逐个扫描每个元组，调用 RI_FKey_check_ins（插             
	 * 入触发器），如同该元组刚刚插入；若有失败，       
	 * 就 ereport(ERROR)。          
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
		 * 调用触发器函数              
		 *
		 * 不传参数，但确实设置了一个上下文 
		 */
		MemSet(fcinfo, 0, SizeForFunctionCallInfo(0));

		/*
		 * 假定 RI_FKey_check_ins 不会查看 flinfo...    
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
 *		创建实现给定外键的插入（on_insert=true）或          
 *		更新 "check" 触发器。   
 *
 * 返回所创建触发器的 OID。        
 */
static Oid
CreateFKCheckTrigger(Oid myRelOid, Oid refRelOid, Constraint *fkconstraint,
					 Oid constraintOid, Oid indexOid, Oid parentTrigOid,
					 bool on_insert)
{
	ObjectAddress trigAddress;
	CreateTrigStmt *fk_trigger;

	/*
	 * 注意：对于自引用外键（引用表与被引用表相          
	 * 同），ON UPDATE 动作在 CHECK 动作之前触发                
	 * 这一点很重要，因为在 UPDATE 事件期间两个触          
	 * 发器都会在同一行上触发；否则 CHECK 触发器            
	 * 将检查该行的非最终状态。触发器按名称顺序        
	 * 触发，动作触发器命名为 "RI_ConstraintTrigger_a_NNNN"，    
	 * 检查触发器名为 "RI_ConstraintTrigger_c_NNNN"。   
	 */
	fk_trigger = makeNode(CreateTrigStmt);
	fk_trigger->replace = false;
	fk_trigger->isconstraint = true;
	fk_trigger->trigname = "RI_ConstraintTrigger_c";
	fk_trigger->relation = NULL;

	/* ON INSERT 或 ON UPDATE       */
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

	/* 让迄今的更改可见    */
	CommandCounterIncrement();

	return trigAddress.objectId;
}

/*
 * createForeignKeyActionTriggers
 *		创建在被引用侧实现外键的"action"触发器。             
 *		    
 *
 * 在 *deleteTrigOid 与 updateTrigOid 中返回新建触发器    
 * *的 OID。    
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
	 * 构建并执行针对被引用表 ON DELETE 动作的             
	 * CREATE CONSTRAINT TRIGGER 语句。   
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

	/* 让迄今的更改可见    */
	CommandCounterIncrement();

	/*
	 * 构建并执行针对被引用表 ON UPDATE 动作的             
	 * CREATE CONSTRAINT TRIGGER 语句。   
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
 *		创建在引用侧实现外键的"check"触发器。                 
 *		    
 *
 * 在 *insertTrigOid 与 updateTrigOid 中返回新建触发器    
 * *的 OID。    
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
 * 更改表删除约束      
 *
 * 同 DROP COLUMN，这里无法使用常规的 ALTER TABLE 递归机制。 
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

	/* 由于本函数会递归，因此可能被驱动至栈溢出       */
	check_stack_depth();

	/* 顶层权限检查已在 ATPrepCmd 做过，否则再做         */
	if (recursing)
		ATSimplePermissions(AT_DropConstraint, rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	conrel = table_open(ConstraintRelationId, RowExclusiveLock);

	/*
	 * 查找并删除目标约束        
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

	/* 最多只能有一行匹配           */
	if (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress conobj;

		con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* 不要删除继承而来的约束*/
		if (con->coninhcount > 0 && !recursing)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop inherited constraint \"%s\" of relation \"%s\"",
							constrName, RelationGetRelationName(rel))));

		is_no_inherit_constraint = con->connoinherit;
		contype = con->contype;

		/*
		 * 若是外键约束，我们最好锁定被引用表并检查它  
		 * 是否正被使用，就像之前对被约束表做过的那样   
		 * （否则我们可能会，比如，删除一个还有未触发事 
		 * 器）。但自引用时可以且必须跳过这一步。  
		 *                       
		 */
		if (contype == CONSTRAINT_FOREIGN &&
			con->confrelid != RelationGetRelid(rel))
		{
			Relation	frel;

			/* 与 RemoveTriggerById 的锁匹配：       */
			frel = table_open(con->confrelid, AccessExclusiveLock);
			CheckAlterTableIsSafe(frel);
			table_close(frel, NoLock);
		}

		/*
		 * 实际执行约束删除              
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
	 * 对于分区表，非 CHECK 的继承约束经依赖机制删除，    
	 * 因此到此就完成了。                  
	 */
	if (contype != CONSTRAINT_CHECK &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		table_close(conrel, RowExclusiveLock);
		return;
	}

	/*
	 * 酌情向子表传播，但与多数 ALTER 例程不同，    
	 * 我们必须一次只做一层这种递归；无法用                 
	 * find_all_inheritors 一次遍历完成。    
	 */
	if (!is_no_inherit_constraint)
		children = find_inheritance_children(RelationGetRelid(rel), lockmode);
	else
		children = NIL;

	/*
	 * 对于分区表，若存在分区而又被要求不递归，       
	 * 那么就是用户错误。约束只定义在父表上并不合理，  
	 * 尤其是当它本身就是一张分区表时。                      
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

		/* find_inheritance_children 已获得锁     */
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

		/* 最多只能有一行匹配           */
		if (!HeapTupleIsValid(tuple = systable_getnext(scan)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" of relation \"%s\" does not exist",
							constrName,
							RelationGetRelationName(childrel))));

		copy_tuple = heap_copytuple(tuple);

		systable_endscan(scan);

		con = (Form_pg_constraint) GETSTRUCT(copy_tuple);

		/* 目前只有 CHECK 约束才能被继承          */
		if (con->contype != CONSTRAINT_CHECK)
			elog(ERROR, "inherited constraint is not a CHECK constraint");

		if (con->coninhcount <= 0)	/* 不应发生     */
			elog(ERROR, "relation %u has non-inherited constraint \"%s\"",
				 childrelid, constrName);

		if (recurse)
		{
			/*
			 * 子约束若有其他来源，只递减继承计数；    
			 * 如果没有，则递归删除它。                          
			 */
			if (con->coninhcount == 1 && !con->conislocal)
			{
				/* 现在也该删除这个子约束了      */
				ATExecDropConstraint(childrel, constrName, behavior,
									 true, true,
									 false, lockmode);
			}
			else
			{
				/* 子约束必须在我删除后仍然存活*/
				con->coninhcount--;
				CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

				/* 让更新可见     */
				CommandCounterIncrement();
			}
		}
		else
		{
			/*
			 * 若被要求仅本表 DROP（不递归），则需将继承  
			 * 者约束标记为本地定义，而非被继承的。     
			 *                       
			 */
			con->coninhcount--;
			con->conislocal = true;

			CatalogTupleUpdate(conrel, &copy_tuple->t_self, copy_tuple);

			/* 让更新可见     */
			CommandCounterIncrement();
		}

		heap_freetuple(copy_tuple);

		table_close(childrel, NoLock);
	}

	table_close(conrel, RowExclusiveLock);
}

/*
 * 更改列类型  
 *
 * 与其他子命令类型不同，我们在阶段 1 就对 ALTER COLUMN    
 * TYPE 做解析变换 --- 传入的 AlterTableCmd 已被变         
 * 换过（也必须如此，因为依赖某些变换后的字段）    
 *
 * 这样做的要点是：一张表的所有 ALTER COLUMN TYPE 的执行 
 * 将在阶段 3 中"并行"执行，因此所有 USING 表达式  
 * 都应假定原始列类型来解析。此外，这也让 USING 表达 
 * 式引用一个将被删除的字段。                                 
 *
 * 要安全地实现，AT_PASS_DROP 然后是 AT_PASS_ALTER_TYPE 必须 
 * 成为阶段 2 的前两个执行步骤；它们不允许见到        
 * 任何其他子命令类型的效果，因为 USING 表达式依据  
 * 未修改的表状态解析。       
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

	/* 查找该属性以便检查其继承状态              */
	tuple = SearchSysCacheAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = attTup->attnum;

	/* 无法修改系统属性       */
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"",
						colName)));

	/*
	 * 修改生成列的类型时不允许指定 USING，因为那样会    
	 * 违反生成表达式。                     
	 */
	if (attTup->attgenerated && def->cooked_default)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_DEFINITION),
				 errmsg("cannot specify USING when altering type of generated column"),
				 errdetail("Column \"%s\" is a generated column.", colName)));

	/*
	 * 不修改继承而来的列。在外层，最好不存在任何继承  
	 * 的定义；递归时，我们假定这一点已在父层检查过     
	 * （见下文）。           
	 */
	if (attTup->attinhcount > 0 && !recursing)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter inherited column \"%s\"",
						colName)));

	/* 不要修改分区键中用到的列          */
	if (has_partition_attrs(rel,
							bms_make_singleton(attnum - FirstLowInvalidHeapAttributeNumber),
							&is_expr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot alter column \"%s\" because it is part of the partition key of relation \"%s\"",
						colName, RelationGetRelationName(rel))));

	/* 查找目标类型      */
	typenameTypeIdAndMod(NULL, typeName, &targettype, &targettypmod);

	aclresult = object_aclcheck(TypeRelationId, targettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, targettype);

	/* 以及排序规则*/
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/* 确保数据类型对列而言合法     */
	CheckAttributeType(colName, targettype, targetcollid,
					   list_make1_oid(rel->rd_rel->reltype),
					   0);

	if (tab->relkind == RELKIND_RELATION ||
		tab->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * 建立表达式把旧数据值变换为新类型。若指定   
		 * USING 选项，用经 transformAlterTableStmt 变换    
		 * 过的表达式，否则取旧值并尝试强制转换。     
		 * 先做此步，好在白费力气前发现不兼容，    
		 * 并且因为表达式需要按原始表的行类型来解析。    
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
			/* 错误消息文本取决于是否指定了 USING         */
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

		/* 最后再修复排序规则   */
		assign_expr_collations(pstate, transform);

		/* 现在就规划表达式，以便准确评估重写必要性。    */
		transform = (Node *) expression_planner((Expr *) transform);

		/*
		 * 添加工作队列项，让 ATRewriteTable 更新列内容。 
		 *          
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
		 * 无存储的关系现在就做这项检查。普通表会推     
		 * 迟到表被重写时再做这项检查。            
		 */
		find_composite_type_dependencies(rel->rd_rel->reltype, rel, NULL);
	}

	ReleaseSysCache(tuple);

	/*
	 * 必要时为每个子表排队新命令手动递归。       
	 * 这里无法应用 ATSimpleRecursion，因为如果有 USING        
	 * 表达式，就需要重映射其中的属性编号。  
	 *
	 * 若被告知不要递归，最好没有任何子表；否则    
	 * 这次修改会使它们失去同步。           
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
		 * find_all_inheritors 会对继承层次结构做递归搜索，  
		 * 所以我们只需处理它返回的列表中的所有 relid。    
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

			/* find_all_inheritors 已获得锁     */
			childrel = relation_open(childrelid, NoLock);
			CheckAlterTableIsSafe(childrel);

			/*
			 * 验证子表没有该列的任何来自此继承层次结构   
			 * 之外的继承定义。（renameatt 以别种方式做了类 
			 * 似的测试，因为它的递归机制不同。）       
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
			 * 重映射属性编号。若未指定 USING 表达式， 
			 * 则不需要这一步。                  
			 */
			if (def->cooked_default)
			{
				AttrMap    *attmap;
				bool		found_whole_row;

				/* 建一份待涂改的副本  */
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
 * 当一列的数据类型被改变时，如果新类型与旧类型足     
 * 够地相同，且 USING 子句并不试图插入别的值，那么    
 * 就可能不需要重写。以下情形可安全跳过重写：       
 *                        
 *
 * - 旧类型可二进制兼容转换为新类型   
 * - 新类型是旧类型之上的无约束域               
 * - {NEW,OLD} 或 {OLD,NEW} 为 {timestamptz,timestamp} 且时区为 UTC     
 *
 * 对于有约束的域，本可以只扫描表并检查约束而不真 
 * 正重写它，但目前没有尝试这样做。                           
 *                                
 */
static bool
ATColumnChangeRequiresRewrite(Node *expr, AttrNumber varattno)
{
	Assert(expr != NULL);

	for (;;)
	{
		/* 仅一个 varno，无需检查此项     */
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
 * 更改列..更改数据类型
 *
 * 返回被修改列的地址。            
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
	 * 若在重写表，就清除所有缺失值，因为重写会         
	 * 使其失去意义。  
	 */
	if (tab->rewrite)
	{
		Relation	newrel;

		newrel = table_open(RelationGetRelid(rel), NoLock);
		RelationClearMissing(newrel);
		relation_close(newrel, NoLock);
		/* 确保与后续的属性修改不发生冲突                  */
		CommandCounterIncrement();
	}

	attrelation = table_open(AttributeRelationId, RowExclusiveLock);

	/* 查找目标列           */
	heapTup = SearchSysCacheCopyAttName(RelationGetRelid(rel), colName);
	if (!HeapTupleIsValid(heapTup)) /* 不应发生     */
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						colName, RelationGetRelationName(rel))));
	attTup = (Form_pg_attribute) GETSTRUCT(heapTup);
	attnum = attTup->attnum;
	attOldTup = TupleDescAttr(tab->oldDesc, attnum - 1);

	/* 检查同列是否多个 ALTER TYPE --- 无法处理        */
	if (attTup->atttypid != attOldTup->atttypid ||
		attTup->atttypmod != attOldTup->atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter type of column \"%s\" twice",
						colName)));

	/* 查找目标类型（不应失败，prep 已找到该类型） */
	typeTuple = typenameType(NULL, typeName, &targettypmod);
	tform = (Form_pg_type) GETSTRUCT(typeTuple);
	targettype = tform->oid;
	/* 以及排序规则*/
	targetcollid = GetColumnDefCollation(NULL, def, targettype);

	/*
	 * 若该列有默认值表达式，取到它并确保能将其         
	 * 强制转换为新数据类型。（必须在改列类型之前     
	 * 做这件事，因为 build_column_default 自身会尝试强   
	 * 制转换，失败时不会发出想要的错误消息。）      
	 *
	 * 去掉旧默认值表达式顶层的隐式强制转换步骤；  
	 * 这已经过商定，满足最小惊讶原则。（向新列类     
	 * 型的转换应当表现得像是从用户所见到的存储表达式  
	 * 开始，而且那些隐式强制转换不会被显示出来。）  
	 *                                              
	 */
	if (attTup->atthasdef)
	{
		defaultexpr = build_column_default(rel, attnum);
		Assert(defaultexpr);
		defaultexpr = strip_implicit_coercions(defaultexpr);
		defaultexpr = coerce_to_target_type(NULL,	/* 无 UNKNOWN 参数*/
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
	 * 找出所有依赖该列的对象（约束、索引等），记录足  
	 * 够的信息，让我们能够重建这些对象。          
	 *
	 * 实际重建并不发生在这里，而是执行完所有单独   
	 * 的 ALTER TYPE 操作之后。不过必须在执行                 
	 * ALTER TYPE 之前就保存信息，否则反解析器会            
	 * 糊涂。    
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
						 * 这一定是 SERIAL 列的序列。              
						 * 我们无需动它。 
						 */
						Assert(foundObject.objectSubId == 0);
					}
					else
					{
						/* 未预期会有任何其他直接依赖...     */
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
				 * 解析后函数体若引用某列，新式 SQL 函数   
				 * 可能依赖它。理想是用反解析加重解析来    
				 * 自动更新函数，但风险大且多半失败。    
				 *                                                           
				 * FIXME。      
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
				 * 视图/规则主体问题与函数主体相同。   
				 * FIXME。                        
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
				 * 触发器可依赖列：被指定为更新目标，   
				 * 或被用于触发器的 WHEN 条件。第一种情    
				 * 况无需额外工作，但第二种情况需更新 WHEN   
				 * 表达式，问题同上。既然无法轻易分辨  
				 * 哪种情况适用，就对两者都暂且搁置。     
				 *                                                        
				 * FIXME。                                  
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
				 * 策略可依赖某列：该列出现在策略的     
				 * USING 或 WITH CHECK qual 表达式中。         
				 * 或许可以重写并复查该表达式，但眼下先   
				 * 搁置。删除重建策略很容易，日后处理。  
				 * FIXME。                                                   
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
						 * 忽略该列自身的默认值表达式，       
						 * 下面再处理它。   
						 */
						Assert(defaultexpr);
					}
					else
					{
						/*
						 * 这应是同表中某生成列表达式发来的 
						 * 引用。SQL 标准禁止修改被生成列  
						 * 的类型，眼下搁置。多思考多努力  
						 * 或许能做到。                                 
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
				 * 给扩展统计机制机会，修复这次列类型改动 
				 * 可能破坏的东西。                 
				 */
				RememberStatisticsForRebuilding(foundObject.objectId, tab);
				break;

			case OCLASS_PUBLICATION_REL:

				/*
				 * PUBLICATION ... FOR TABLE ... WHERE 子句列引用。   
				 * 问题与上述相同。FIXME。              
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
				 * 未预期这类对象中有任何一个会依赖列。    
				 *          
				 */
				elog(ERROR, "unexpected object depending on column: %s",
					 getObjectDescription(&foundObject, false));
				break;

				/*
				 * 这里有意不设 default: 分支；                   
				 * 希望编译器在上面未处理新 OCLASS 时告警。   
				 */
		}
	}

	systable_endscan(scan);

	/*
	 * 现在扫描该列对其他东西的依赖。我们应当找到的 
	 * 唯一应找到的是该列数据类型的依赖，可能还有    
	 * 排序规则的依赖。这些依赖可以被删除。 
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
	 * 现在动手 --- 修改已记录的列类型与排序规则（注   
	 * 意 heapTup 是 syscache 条目的副本，因此可以涂改。）    
	 * 先修缺失值（如有）。  
	 */
	if (attTup->atthasmissing)
	{
		Datum		missingval;
		bool		missingNull;

		/* 如果 rewrite 为真，缺失值应当已被清除            */
		Assert(tab->rewrite == 0);

		/* 获取缺失值 datum       */
		missingval = heap_getattr(heapTup,
								  Anum_pg_attribute_attmissingval,
								  attrelation->rd_att,
								  &missingNull);

		/* 如果是空数组则无事可做           */

		if (!missingNull)
		{
			/*
			 * 把 datum 从数组取出，重新打包到一个用新类 
			 * 型数据新建的数组里。既然表不需重写，实   
			 * 际的 Datum 无需改动，改的只是数组元数据。 
			 *                                  
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

	/* 安装对新数据类型与排序规则的依赖   */
	add_column_datatype_dependency(RelationGetRelid(rel), attnum, targettype);
	add_column_collation_dependency(RelationGetRelid(rel), attnum, targetcollid);

	/*
	 * 删除该列任何 pg_statistic 条目，因其类型现已错误    
	 */
	RemoveStatistics(RelationGetRelid(rel), attnum);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel), attnum);

	/*
	 * 用蛮力方式更新默认值（如果存在）--- 删除默认值 
	 * 然后再重新添加。走捷径可能不安全，因为新版本很 
	 * 可能有额外依赖。（现在就做是可行的，而非      
	 * 在其他 ALTER TYPE 命令之后做，因为默认值不依赖其  
	 * 他列类型。）            
	 */
	if (defaultexpr)
	{
		/*
		 * 如果这是 GENERATED 默认值，就删除其依赖记    
		 * 录，尤其该列的 INTERNAL 依赖，否则 dependency.c  
		 * 会拒绝执行这次删除。                                 
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
		 * 让迄今的更新可见，尤其是新的 pg_attribute 行，它 
		 * 随后会再次更新。    
		 */
		CommandCounterIncrement();

		/*
		 * 这里出于安全考虑使用 RESTRICT，但目前并不       
		 * 预期有对象依赖默认值。 
		 */
		RemoveAttrDefault(RelationGetRelid(rel), attnum, DROP_RESTRICT, true,
						  true);

		StoreAttrDefault(rel, attnum, defaultexpr, true, false);
	}

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);

	/* 清理  */
	heap_freetuple(heapTup);

	return address;
}

/*
 * ATExecAlterColumnType 的子例程：记住需要重置               
 * 复制标识。   
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
 * ATExecAlterColumnType 的子例程：记住任何聚簇索引。   
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
 * ATExecAlterColumnType 的子例程：记住某个约束需要         
 * 重建（这点我们可能已知）。     
 */
static void
RememberConstraintForRebuilding(Oid conoid, AlteredTableInfo *tab)
{
	/*
	 * 这个去重检查因两个相互独立的理由而关键：         
	 * 我们不允许尝试重建同一个约束两次；而且如果某    
	 * 个约束依赖多个将改类型的列，我们就必须在        
	 * 应用任何列类型改动之前捕捉它的定义字符串。     
	 * 如果之后再次询问，ruleutils.c 会糊涂。           
	 */
	if (!list_member_oid(tab->changedConstraintOids, conoid))
	{
		/* 好，捕捉该约束现有的定义字符串           */
		char	   *defstring = pg_get_constraintdef_command(conoid);
		Oid			indoid;

		tab->changedConstraintOids = lappend_oid(tab->changedConstraintOids,
												 conoid);
		tab->changedConstraintDefs = lappend(tab->changedConstraintDefs,
											 defstring);

		/*
		 * 对约束的索引（如有），记住它是用于表的复制  
		 * 标识还是聚簇索引，这样 ATPostAlterTypeCleanup() 就能  
		 * 把恢复这些属性所需的命令排队加入。                
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
 * ATExecAlterColumnType 的子例程：记住某个索引需要     
 * 重建（这点我们可能已知）。     
 */
static void
RememberIndexForRebuilding(Oid indoid, AlteredTableInfo *tab)
{
	/*
	 * 这个去重检查因两个相互独立的理由而关键：         
	 * 我们不允许尝试重建同一个索引两次；而且如果某   
	 * 个索引依赖多个将改类型的列，我们就必须在        
	 * 应用任何列类型改动之前捕捉它的定义字符串。      
	 * 如果之后再次询问，ruleutils.c 会糊涂。 
	 */
	if (!list_member_oid(tab->changedIndexOids, indoid))
	{
		/*
		 * 把它加为待重建索引前，最好先看它是否属于  
		 * 某约束；如是，就改为重建该约束。通常此检      
		 * 查会失败，因为约束索引通常只依赖约束。但   
		 * 这种索引也可能对表列有直接依赖，例如带         
		 * WHERE 子句的部分排除约束。                            
		 *                                                 
		 */
		Oid			conoid = get_index_constraint(indoid);

		if (OidIsValid(conoid))
		{
			RememberConstraintForRebuilding(conoid, tab);
		}
		else
		{
			/* 好，捕捉该索引现有的定义字符串      */
			char	   *defstring = pg_get_indexdef_string(indoid);

			tab->changedIndexOids = lappend_oid(tab->changedIndexOids,
												indoid);
			tab->changedIndexDefs = lappend(tab->changedIndexDefs,
											defstring);

			/*
			 * 记住该索引用于表的复制标识还是聚簇索引，   
			 * 这样 ATPostAlterTypeCleanup() 就能把恢复这些属性    
			 * 所需的命令排队加入。                              
			 */
			RememberReplicaIdentityForRebuilding(indoid, tab);
			RememberClusterOnForRebuilding(indoid, tab);
		}
	}
}

/*
 * ATExecAlterColumnType 的子例程：记住某个统计对象需要    
 * 重建（这点我们可能已知）。           
 */
static void
RememberStatisticsForRebuilding(Oid stxoid, AlteredTableInfo *tab)
{
	/*
	 * 这个去重检查因两个相互独立的理由而关键：         
	 * 我们不允许尝试重建同一个统计对象两次；而且如  
	 * 果该统计对象依赖多个将改类型的列，我们就         
	 * 必须在应用类型改动之前捕捉其定义字符串。         
	 * 如果之后再次询问，ruleutils.c 会糊涂。                   
	 */
	if (!list_member_oid(tab->changedStatisticsOids, stxoid))
	{
		/* 好，捕捉该统计对象现有的定义字符串            */
		char	   *defstring = pg_get_statisticsobjdef_string(stxoid);

		tab->changedStatisticsOids = lappend_oid(tab->changedStatisticsOids,
												 stxoid);
		tab->changedStatisticsDefs = lappend(tab->changedStatisticsDefs,
											 defstring);
	}
}

/*
 * 在完成某个关系的全部 ALTER TYPE 操作之后进行清 
 * 理。必须删除并重建所有依赖被改列的索引与      
 * 约束。实际删除在此完成，重建则通过添加工  
 * 作队列项、留待以后执行那些步骤来管理的。   
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
	 * 收集所有待删除的约束与索引，以便我们能在单次调 
	 * 用中处理它们。这样就无需担心它们之间的依赖。   
	 *            
	 */
	objects = new_object_addresses();

	/*
	 * 重新解析索引与约束定义，并把它们附加到相应      
	 * 的工作队列项上。之所以在删除前这样做，是因为    
	 * 另一表上的约束，我们可能还没持有该约束所挂 
	 * 的表上的排他锁，而必须在重新解析/删除之前拿到   
	 * 它。（至少 FOREIGN KEY、CHECK 与 EXCLUSION 约束是          
	 * 这样；非 FK 情形则要求另一表约束表达式中有    
	 * 对目标表复合类型的依赖。）                                
	 *                                 
	 *
	 * 无法依赖反解析输出来判断要操作哪个关系，         
	 * 因为并发活动可能已让名字解析成不同对象。    
	 * 必须用正在处理的约束或索引的 OID 来确         
	 * 定要操作哪个关系。                                         
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
		if (!HeapTupleIsValid(tup)) /* 不应发生      */
			elog(ERROR, "cache lookup failed for constraint %u", oldId);
		con = (Form_pg_constraint) GETSTRUCT(tup);
		if (OidIsValid(con->conrelid))
			relid = con->conrelid;
		else
		{
			/* 必须是域约束          */
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
		 * 若该约束是（仅）继承而来，我们不想在此注入 
		 * 新定义；当 ATAddCheckConstraint 因添加父表的约束     
		 * 递归时会被重建。但须把信息带到这里，才能在   
		 * 下面删除该约束。                                         
		 */
		if (!conislocal)
			continue;

		/*
		 * 重建引用被改之表的另一表约束时，可能        
		 * 还没持有那个表的锁，所以现在就获取一个。      
		 * DROP CONSTRAINT 步骤需要 AccessExclusiveLock，因此          
		 * 请求更弱的锁没有任何价值。                            
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
		 * 同上，若索引所在表非同一表，需确保我们持有  
		 * 它的锁。   
		 */
		if (relid != tab->relid)
			LockRelationOid(relid, AccessExclusiveLock);

		ATPostAlterTypeParse(oldId, relid, InvalidOid,
							 (char *) lfirst(def_item),
							 wqueue, lockmode, tab->rewrite);

		ObjectAddressSet(obj, RelationRelationId, oldId);
		add_exact_object_address(&obj, objects);
	}

	/* 为新统计对象添加依赖      */
	forboth(oid_item, tab->changedStatisticsOids,
			def_item, tab->changedStatisticsDefs)
	{
		Oid			oldId = lfirst_oid(oid_item);
		Oid			relid;

		relid = StatisticsGetRelation(oldId, false);

		/*
		 * 同上，若统计对象所在表非同一表，需确保持有  
		 * 其锁。不过这里取的是               
		 * ShareUpdateExclusiveLock，与 CreateStatistics 和                
		 * RemoveStatisticsById 锁级别一致。   
		 *
		 * 注意：应在所有获取                           
		 * AccessExclusiveLock 的情形之后做，否则提升表锁      
		 * 而有死锁风险。     
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
	 * 把恢复复制标识索引标记的命令排队加入    
	 */
	if (tab->replicaIdentityIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);
		ReplicaIdentityStmt *subcmd = makeNode(ReplicaIdentityStmt);

		subcmd->identity_type = REPLICA_IDENTITY_INDEX;
		subcmd->name = tab->replicaIdentityIndex;
		cmd->subtype = AT_ReplicaIdentity;
		cmd->def = (Node *) subcmd;

		/* 在索引和约束之后再做      */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * 把恢复用于聚簇的索引标记的命令排队加入。  
	 */
	if (tab->clusterOnIndex)
	{
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_ClusterOn;
		cmd->name = tab->clusterOnIndex;

		/* 在索引和约束之后再做      */
		tab->subcmds[AT_PASS_OLD_CONSTR] =
			lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);
	}

	/*
	 * 在这里使用 DROP_RESTRICT 应没问题，因为不应             
	 * 有对象依赖它们。      
	 */
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	free_object_addresses(objects);

	/*
	 * 这些对象会在后续各轮工作队列处理中被重建。      
	 *       
	 */
}

/*
 * 针对新建立的列数据类型，解析先前保存的约束、索  
 * 引或统计对象的定义字符串，并把得到的命令解析树   
 * 排队以待执行。                                   
 *
 * 举例来说，如果 WHERE 子句使用了新列类型下不可用  
 * 的操作符，这一步就可能失败。            
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
	 * 预期只会得到 ALTER TABLE 与 CREATE INDEX 语句。   
	 * 因此无需经 parse_analyze_*() 或重写器处理， 
	 * 而是要让它们经 parse_utilcmd.c 处理以备执行。        
	 *                                                          
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

	/* 调用者应当已经获取了我们需要的任何锁。  */
	rel = relation_open(oldRelId, NoLock);

	/*
	 * 把每条生成的命令挂到工作队列中合适位置。注     
	 * 意这可能导致创建全新的工作队列项。                   
	 *
	 * 还要注意，必须调整命令子类型，因为事实证明       
	 * 索引与约束重建时行为须与初次创建略有不同    
	 * 。                               
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
			/* 保留该索引的注释 */
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
					/* 保留索引上的任何注释*/
					indstmt->idxcomment = GetComment(indoid,
													 RelationRelationId, 0);
					indstmt->reset_default_tblspc = true;

					cmd->subtype = AT_ReAddIndex;
					tab->subcmds[AT_PASS_OLD_INDEX] =
						lappend(tab->subcmds[AT_PASS_OLD_INDEX], cmd);

					/* 重新创建约束上的任何注释   */
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
					/* 不重写外键的任何一侧 */
					if (con->contype == CONSTR_FOREIGN &&
						!rewrite && tab->rewrite == 0)
						TryReuseForeignKey(oldId, con);
					con->reset_default_tblspc = true;
					cmd->subtype = AT_ReAddConstraint;
					tab->subcmds[AT_PASS_OLD_CONSTR] =
						lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

					/* 重新创建约束上的任何注释   */
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
					 * 解析器会为 PRIMARY KEY 索引/约束列创建  
					 * AT_SetNotNull 子命令，无需在此对它们做任  
					 * 何处理；各列 NOT NULL 标记已传到新表   
					 * 定义中。                                         
					 *                          
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

			if (stmt->subtype == 'C')	/* 添加约束   */
			{
				Constraint *con = castNode(Constraint, stmt->def);
				AlterTableCmd *cmd = makeNode(AlterTableCmd);

				cmd->subtype = AT_ReAddDomainConstraint;
				cmd->def = (Node *) stmt;
				tab->subcmds[AT_PASS_OLD_CONSTR] =
					lappend(tab->subcmds[AT_PASS_OLD_CONSTR], cmd);

				/* 重新创建约束上的任何注释   */
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

			/* 保留统计对象的注释          */
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
 * ATPostAlterTypeParse() 的子例程：为正在重建的表约束     
 * 或域约束重新创建任何已存在的注释。    
 *
 * objid 是该约束的 OID。       
 * 对表约束传递 "rel"；对域约束传递 "domname"（域的        
 * 限定名字符串列表形式）。      
 * （这些信息以及 conname 本可以从 pg_constraint 条目中       
 * 挖出来；但调用者已有它们，不妨直接传递。）  
 */
static void
RebuildConstraintComment(AlteredTableInfo *tab, int pass, Oid objid,
						 Relation rel, List *domname,
						 const char *conname)
{
	CommentStmt *cmd;
	char	   *comment_str;
	AlterTableCmd *newcmd;

	/* 查找所需对象的注释，若无则返回         */
	comment_str = GetComment(objid, ConstraintRelationId, 0);
	if (comment_str == NULL)
		return;

	/* 构建 CommentStmt 节点，复制全部输入数据      */
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

	/* 将其追加到命令列表   */
	newcmd = makeNode(AlterTableCmd);
	newcmd->subtype = AT_ReAddComment;
	newcmd->def = (Node *) cmd;
	tab->subcmds[pass] = lappend(tab->subcmds[pass], newcmd);
}

/*
 * ATPostAlterTypeParse() 的子例程。调用 CheckIndexCompatible()        
 * 做真正的分析，然后根据其结论来修改 IndexStmt。        
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

		/* 如果这是分区索引，就没有可共享的存储。  */
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
 * ATPostAlterTypeParse() 子例程。   
 *
 * 把旧的 P-F 相等操作符存入 Constraint 节点，供               
 * ATAddForeignKeyConstraint() 用来判定能否跳过该约束的         
 * 重新验证。                
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
	Assert(con->old_conpfeqop == NIL);	/* 已准备好该节点      */

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(oldId));
	if (!HeapTupleIsValid(tup)) /* 不应发生      */
		elog(ERROR, "cache lookup failed for constraint %u", oldId);

	adatum = SysCacheGetAttrNotNull(CONSTROID, tup,
									Anum_pg_constraint_conpfeqop);
	arr = DatumGetArrayTypeP(adatum);	/* 确保未 TOAST    */
	numkeys = ARR_DIMS(arr)[0];
	/* 测试同 ri_FetchConstraintInfo() 中的一致  */
	if (ARR_NDIM(arr) != 1 ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	rawarr = (Oid *) ARR_DATA_PTR(arr);

	/* 把操作符 OID 列表存入本 Constraint 节点       */
	for (i = 0; i < numkeys; i++)
		con->old_conpfeqop = lappend_oid(con->old_conpfeqop, rawarr[i]);

	ReleaseSysCache(tup);
}

/*
 * 更改列..选项 ( ... )      
 *
 * 返回被修改列的地址               
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

	/* 首先，确定与外部表关联的 FDW 验证器。           */
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

	/* 阻止它们修改系统属性                */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", colName)));


	/* 为新元组值初始化缓冲区       */
	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	/* 提取当前选项          */
	datum = SysCacheGetAttr(ATTNAME,
							tuple,
							Anum_pg_attribute_attfdwoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* 变换这些选项    */
	datum = transformGenericOptions(AttributeRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_attribute_attfdwoptions - 1] = datum;
	else
		repl_null[Anum_pg_attribute_attfdwoptions - 1] = true;

	repl_repl[Anum_pg_attribute_attfdwoptions - 1] = true;

	/* 一切正常 - 更新该元组           */

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
 * 当我们从表递归到它的索引、序列或 TOAST 表时，  
 * recursing 为真。不允许那些东西的所有权与父表分开        
 * 被更改。此外，我们可以跳过权限检查（这不仅是优化  
 * ，而是必需，否则将无法正确处理 TOAST 表）。        
 *                               
 *
 * 如果 ALTER TYPE OWNER 调用我们修复独立复合类型        
 * 时，recursing 也为真。 
 */
void
ATExecChangeOwner(Oid relationOid, Oid newOwnerId, bool recursing, LOCKMODE lockmode)
{
	Relation	target_rel;
	Relation	class_rel;
	HeapTuple	tuple;
	Form_pg_class tuple_class;

	/*
	 * 对目标表获取持有到事务结束的排他锁。使用       
	 * relation_open，这样我们就能处理索引和序列。 
	 */
	target_rel = relation_open(relationOid, lockmode);

	/* 也取 pg_class 元组      */
	class_rel = table_open(RelationRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relationOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relationOid);
	tuple_class = (Form_pg_class) GETSTRUCT(tuple);

	/* 能改变此元组的所有权吗？       */
	switch (tuple_class->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_VIEW:
		case RELKIND_MATVIEW:
		case RELKIND_FOREIGN_TABLE:
		case RELKIND_PARTITIONED_TABLE:
			/* 可改变所有者 */
			break;
		case RELKIND_INDEX:
			if (!recursing)
			{
				/*
				 * 过去允许使用 ALTER INDEX OWNER，且旧版         
				 * 本 pg_dump 会生成它，所以我们只警告，       
				 * 而不报错退出。另外为免恢复旧转储时的  
				 * 无谓聒噪，若该命令反正就是无操作，     
				 * 就什么都不说。                                 
				 */
				if (tuple_class->relowner != newOwnerId)
					ereport(WARNING,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("cannot change owner of index \"%s\"",
									NameStr(tuple_class->relname)),
							 errhint("Change the ownership of the index's table instead.")));
				/* 借无操作路径退出的技巧     */
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
				/* 如果是被拥有的序列，则不允许单独修改它 */
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
	 * 若新所有者与现有所有者相同，就把命令视为    
	 * 成功。这样做是为了转储恢复的目的。               
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

		/* 递归到索引或 TOAST 表时跳过权限检查             */
		if (!recursing)
		{
			/* 超级用户总是可以做 */
			if (!superuser())
			{
				Oid			namespaceOid = tuple_class->relnamespace;
				AclResult	aclresult;

				/* 否则，必须是现有对象的所有者      */
				if (!object_ownercheck(RelationRelationId, relationOid, GetUserId()))
					aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relationOid)),
								   RelationGetRelationName(target_rel));

				/* 必须能够成为新所有者   */
				check_can_set_role(GetUserId(), newOwnerId);

				/* 新所有者必须对命名空间有 CREATE 权限*/
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
		 * 确定新所有者的 ACL 改动；仅非空时需要。   
		 *                                    
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
		 * 同样需更新各按列 ACL 以反映新所有者；为整洁 
		 * 起见拆分为独立子例程实现。                      
		 */
		change_owner_fix_column_acls(relationOid,
									 tuple_class->relowner,
									 newOwnerId);

		/*
		 * 更新所有者依赖引用（如有）。复合类型没有： 
		 * 其跟踪在 pg_type 条目上；索引和 TOAST 表也没       
		 * 有各自的条目。                                        
		 */
		if (tuple_class->relkind != RELKIND_COMPOSITE_TYPE &&
			tuple_class->relkind != RELKIND_INDEX &&
			tuple_class->relkind != RELKIND_PARTITIONED_INDEX &&
			tuple_class->relkind != RELKIND_TOASTVALUE)
			changeDependencyOnOwner(RelationRelationId, relationOid,
									newOwnerId);

		/*
		 * 同时改变表的行类型的所有权（如果有的话）    
		 */
		if (OidIsValid(tuple_class->reltype))
			AlterTypeOwnerInternal(tuple_class->reltype, newOwnerId);

		/*
		 * 若操作对象是表或物化视图，还要一并改变该关 
		 * 系的全部索引与序列所有权，连同其 TOAST 表   
		 * （如果有的话）。                             
		 */
		if (tuple_class->relkind == RELKIND_RELATION ||
			tuple_class->relkind == RELKIND_PARTITIONED_TABLE ||
			tuple_class->relkind == RELKIND_MATVIEW ||
			tuple_class->relkind == RELKIND_TOASTVALUE)
		{
			List	   *index_oid_list;
			ListCell   *i;

			/* 查找属于该关系的所有索引            */
			index_oid_list = RelationGetIndexList(target_rel);

			/* 对每个索引递归改变其所有权          */
			foreach(i, index_oid_list)
				ATExecChangeOwner(lfirst_oid(i), newOwnerId, true, lockmode);

			list_free(index_oid_list);
		}

		/* 如果有 TOAST 表，递归改变其所有权           */
		if (tuple_class->reltoastrelid != InvalidOid)
			ATExecChangeOwner(tuple_class->reltoastrelid, newOwnerId,
							  true, lockmode);

		/* 如果有依赖序列，也递归改变它们的所有权 */
		change_owner_recurse_to_sequences(relationOid, newOwnerId, lockmode);
	}

	InvokeObjectPostAlterHook(RelationRelationId, relationOid, 0);

	ReleaseSysCache(tuple);
	table_close(class_rel, RowExclusiveLock);
	relation_close(target_rel, NoLock);
}

/*
 * change_owner_fix_column_acls
 *
 * ATExecChangeOwner 的辅助函数。扫描表的各列，修正      
 * 所有非空的列 ACL 以反映新所有者。           
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

		/* 忽略已删除的列  */
		if (att->attisdropped)
			continue;

		aclDatum = heap_getattr(attributeTuple,
								Anum_pg_attribute_attacl,
								RelationGetDescr(attRelation),
								&isNull);
		/* 空 ACL 不需要更改          */
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
 * ATExecChangeOwner 的辅助函数。检查 pg_depend，查找       
 * 依赖 serial 列的序列，并改变它们的所有权。          
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
	 * SERIAL 序列是对该表某列有 auto 依赖的序列（        
	 * 我们并不关心具体是*哪一*列）。            
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
	/* 我们不指定 refobjsubid      */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* 跳过除对列的 auto 依赖之外的其他依赖       */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* 使用 relation_open 以防它是索引      */
		seqRel = relation_open(depForm->objid, lockmode);

		/* 跳过非序列关系       */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* 无需保留该锁       */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* 修改序列期间无需关闭它。                   */
		ATExecChangeOwner(depForm->objid, newOwnerId, true, lockmode);

		/* 现在可以关闭它了。锁保留到事务结束。       */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}

/*
 * ALTER TABLE CLUSTER ON
 *
 * 我们唯一要做的是修改 indisclustered 位。              
 *
 * 返回新聚簇索引的地址。              
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

	/* 检查索引可用于聚簇        */
	check_index_is_clusterable(rel, indexOid, lockmode);

	/* 然后执行    */
	mark_index_clustered(rel, indexOid, false);

	ObjectAddressSet(address,
					 RelationRelationId, indexOid);

	return address;
}

/*
 * 更改表设置无聚簇       
 *
 * 需要找出表上设置了 indisclustered 位的所有索引并关  
 * 闭该位。        
 */
static void
ATExecDropCluster(Relation rel, LOCKMODE lockmode)
{
	mark_index_clustered(rel, InvalidOid, false);
}

/*
 * SET ACCESS METHOD 的准备阶段      
 *
 * 检查访问方法是否存在。如果与表当前的访问方法相     
 * 同，则什么都不做。否则需要进行表重写。              
 */
static void
ATPrepSetAccessMethod(AlteredTableInfo *tab, Relation rel, const char *amname)
{
	Oid			amoid;

	/* 检查表的访问方法是否存在      */
	amoid = get_table_am_oid(amname, false);

	if (rel->rd_rel->relam == amoid)
		return;

	/* 保存信息供阶段 3 执行实际工作*/
	tab->rewrite |= AT_REWRITE_ACCESS_METHOD;
	tab->newAccessMethod = amoid;
}

/*
 * 更改表设置表空间  
 */
static void
ATPrepSetTableSpace(AlteredTableInfo *tab, Relation rel, const char *tablespacename, LOCKMODE lockmode)
{
	Oid			tablespaceId;

	/* 检查表空间是否存在      */
	tablespaceId = get_tablespace_oid(tablespacename, false);

	/* 除移动到数据库默认表空间外都检查权限     */
	if (OidIsValid(tablespaceId) && tablespaceId != MyDatabaseTableSpace)
	{
		AclResult	aclresult;

		aclresult = object_aclcheck(TableSpaceRelationId, tablespaceId, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE, tablespacename);
	}

	/* 保存信息供阶段 3 执行实际工作*/
	if (OidIsValid(tab->newTableSpace))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot have multiple SET TABLESPACE subcommands")));

	tab->newTableSpace = tablespaceId;
}

/*
 * 设置或替换 reloptions。     
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
		return;					/* 无事可做  */

	pgclass = table_open(RelationRelationId, RowExclusiveLock);

	/* 获取堆元组  */
	relid = RelationGetRelid(rel);
	tuple = SearchSysCacheLocked1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	if (operation == AT_ReplaceRelOptions)
	{
		/*
		 * 若应替换 reloptions 列表，就假装之前没有任何     
		 *                        
		 */
		datum = (Datum) 0;
		isnull = true;
	}
	else
	{
		/* 获取旧的 reloptions*/
		datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions,
								&isnull);
	}

	/* 生成新提议的 reloptions（text 数组）*/
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 defList, NULL, validnsps, false,
									 operation == AT_ResetRelOptions);

	/* 校验   */
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

	/* 视图选项的特例校验             */
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
		 * 若指定了检查选项，则查看视图是否可自      
		 * 动更新。                   
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
	 * 这里我们只需更新 pg_class 行；新选项会在提交后的    
	 * 缓存失效过程中传播进 relcache。               
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

	/* 如果存在 TOAST 表，则对 TOAST 表重复整个过程   */
	if (OidIsValid(rel->rd_rel->reltoastrelid))
	{
		Relation	toastrel;
		Oid			toastid = rel->rd_rel->reltoastrelid;

		toastrel = table_open(toastid, lockmode);

		/* 获取堆元组  */
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(toastid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", toastid);

		if (operation == AT_ReplaceRelOptions)
		{
			/*
			 * 若被要求替换 reloptions 列表，则只需假装   
			 * 之前没有任何列表。    
			 */
			datum = (Datum) 0;
			isnull = true;
		}
		else
		{
			/* 获取旧 reloptions   */
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
 * 对没有元组重写要做的情况执行 ALTER TABLE SET          
 * TABLESPACE，因此我们只想尽快复制数据。                       
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
	 * 此处需要锁，以防我们递归到 TOAST 表或索引     
	 */
	rel = relation_open(tableOid, lockmode);

	/* 先检查关系是否可以移动到新表空间       */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel), 0);
		relation_close(rel, NoLock);
		return;
	}

	reltoastrelid = rel->rd_rel->reltoastrelid;
	/* 必要时获取 TOAST 关系上的索引列表           */
	if (OidIsValid(reltoastrelid))
	{
		Relation	toastRel = relation_open(reltoastrelid, lockmode);

		reltoastidxids = RelationGetIndexList(toastRel);
		relation_close(toastRel, lockmode);
	}

	/*
	 * Relfilenumbers 在各数据库间跨表空间并不唯一，因此  
	 * 需在新表空间中分配新值。             
	 */
	newrelfilenumber = GetNewRelFileNumber(newTableSpace, NULL,
										   rel->rd_rel->relpersistence);

	/* 打开新旧关系        */
	newrlocator = rel->rd_locator;
	newrlocator.relNumber = newrelfilenumber;
	newrlocator.spcOid = newTableSpace;

	/* 移交给 AM 实际创建新关系存储并复制数据             */
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
	 * 注意：如果允许在 pg_class 或其索引上执行           
	 * ATExecSetTableSpace()，这将无法工作（上面的复制不会  
	 * 包含更新后的 pg_class 条目），这被        
	 * CheckRelationTableSpaceMove() 
	 */
	SetRelationTableSpace(rel, newTableSpace, newrelfilenumber);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	RelationAssumeNewRelfilelocator(rel);

	relation_close(rel, NoLock);

	/* 确保 reltablespace 的更改可见          */
	CommandCounterIncrement();

	/* 同时移动关联的 TOAST 关系和/或索引    */
	if (OidIsValid(reltoastrelid))
		ATExecSetTableSpace(reltoastrelid, newTableSpace, lockmode);
	foreach(lc, reltoastidxids)
		ATExecSetTableSpace(lfirst_oid(lc), newTableSpace, lockmode);

	/* 清理   */
	list_free(reltoastidxids);
}

/*
 * 针对无存储但又希望保留表空间的关系对 ALTER TABLE  
 * SET TABLESPACE 做特殊处理。                      
 *
 * 由于这些关系没有存储，表空间可用简单的元数据     
 * 操作即可更新表空间。                   
 */
static void
ATExecSetTableSpaceNoStorage(Relation rel, Oid newTableSpace)
{
	/*
	 * 不应在拥有存储的关系上调用；它们在阶段 3 处理。  
	 *         
	 */
	Assert(!RELKIND_HAS_STORAGE(rel->rd_rel->relkind));

	/* 检查关系是否可以移动到其新表空间     */
	if (!CheckRelationTableSpaceMove(rel, newTableSpace))
	{
		InvokeObjectPostAlterHook(RelationRelationId,
								  RelationGetRelid(rel),
								  0);
		return;
	}

	/* 可以更新，故修改 reltablespace      */
	SetRelationTableSpace(rel, newTableSpace, InvalidOid);

	InvokeObjectPostAlterHook(RelationRelationId, RelationGetRelid(rel), 0);

	/* 确保 reltablespace 的更改可见          */
	CommandCounterIncrement();
}

/*
 * Alter Table ALL ... SET TABLESPACE
 *
 * 允许用户把当前数据库中某个表空间内的某类全部对象   
 * 移动到另一个表空间。也可以依据对象的所有者来选择   
 * 对象，让用户只能移动自己的对象。                    
 * 用户必须在新表空间上有 CREATE 权限，照例。主要的        
 * 权限处理由底层的表移动函数完成。                    
 *
 * 所有待移动对象先被加锁。若指定了 NOWAIT 而锁无法    
 * 获取，则我们 ereport(ERROR)。          
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

	/* 确保没被要求移动我们无法移动的东西 */
	if (stmt->objtype != OBJECT_TABLE && stmt->objtype != OBJECT_INDEX &&
		stmt->objtype != OBJECT_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("only tables, indexes, and materialized views exist in tablespaces")));

	/* 获取新旧表空间的 OID         */
	orig_tablespaceoid = get_tablespace_oid(stmt->orig_tablespacename, false);
	new_tablespaceoid = get_tablespace_oid(stmt->new_tablespacename, false);

	/* 共享关系无法移入或移出 pg_global           */
	/* ATExecSetTableSpace 也会检查此项，但早点停止更好        */
	if (orig_tablespaceoid == GLOBALTABLESPACE_OID ||
		new_tablespaceoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot move relations in to or out of pg_global tablespace")));

	/*
	 * 必须拥有新表空间上的 CREATE 权限，除非它是    
	 * 数据库默认表空间（对其隐式拥有 CREATE 权限）。  
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
	 * 检查完成后，看是否该把某个设为 InvalidOid，   
	 * 因为它是我们数据库的默认表空间。           
	 */
	if (orig_tablespaceoid == MyDatabaseTableSpace)
		orig_tablespaceoid = InvalidOid;

	if (new_tablespaceoid == MyDatabaseTableSpace)
		new_tablespaceoid = InvalidOid;

	/* 跳过*/
	if (orig_tablespaceoid == new_tablespaceoid)
		return new_tablespaceoid;

	/*
	 * 遍历表空间中的对象列表并移动。当然，这只会    
	 * 找到我们数据库中的对象。         
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
		 * 不要随此操作移动 pg_catalog 的对象；若管理员  
		 * 确实想这么做，可发出单独的 ALTER 命令。    
		 *                   
		 *
		 * 同时，显式避开所有共享表、临时表或 TOAST 表  
		 * （TOAST 将随主表一起移动）。   
		 */
		if (IsCatalogNamespace(relForm->relnamespace) ||
			relForm->relisshared ||
			isAnyTempNamespace(relForm->relnamespace) ||
			IsToastNamespace(relForm->relnamespace))
			continue;

		/* 只移动所请求的对象类型   */
		if ((stmt->objtype == OBJECT_TABLE &&
			 relForm->relkind != RELKIND_RELATION &&
			 relForm->relkind != RELKIND_PARTITIONED_TABLE) ||
			(stmt->objtype == OBJECT_INDEX &&
			 relForm->relkind != RELKIND_INDEX &&
			 relForm->relkind != RELKIND_PARTITIONED_INDEX) ||
			(stmt->objtype == OBJECT_MATVIEW &&
			 relForm->relkind != RELKIND_MATVIEW))
			continue;

		/* 检查是否只移动由特定角色拥有的对象        */
		if (role_oids != NIL && !list_member_oid(role_oids, relForm->relowner))
			continue;

		/*
		 * 在此处检查权限，因为正在锁定表，也可避免做 
		 * 大量工作后却在半途失败。注意，                      
		 * AlterTableInternal() 也会检查权限。                    
		 *
		 * 调用者必须被视为表的拥有者才能移动它。  
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

		/* 加入待移动对象列表        */
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

	/* 一切都已锁定，循环遍历并移动所有关系。         */
	foreach(l, relations)
	{
		List	   *cmds = NIL;
		AlterTableCmd *cmd = makeNode(AlterTableCmd);

		cmd->subtype = AT_SetTableSpace;
		cmd->name = stmt->new_tablespacename;

		cmds = lappend(cmds, cmd);

		EventTriggerAlterTableStart((Node *) stmt);
		/* OID 由 AlterTableInternal 设置*/
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
	 * 由于我们直接复制文件而不看共享缓冲区，最好先    
	 * 源关系在共享缓冲区中的页面都冲刷出去。我们      
	 * 假设在持有该关系的排他锁期间不会有新的更改       
	 * 发生。                         
	 */
	FlushRelationBuffers(rel);

	/*
	 * 创建并复制关系的所有分支，安排删除旧物理文件  
	 *                    
	 *
	 * 注意：relfilenumber 值的任何冲突都会在         
	 * RelationCreateStorage() 
	 */
	RelationCreateStorage(newrlocator, rel->rd_rel->relpersistence, true);

	/* 复制主分支*/
	RelationCopyStorage(RelationGetSmgr(rel), dstrel, MAIN_FORKNUM,
						rel->rd_rel->relpersistence);

	/* 复制已存在的额外分支    */
	for (ForkNumber forkNum = MAIN_FORKNUM + 1;
		 forkNum <= MAX_FORKNUM; forkNum++)
	{
		if (smgrexists(RelationGetSmgr(rel), forkNum))
		{
			smgrcreate(dstrel, forkNum, false);

			/*
			 * 若关系是持久的，或这是不记录 WAL 关系的      
			 * init 分支。                    
			 */
			if (RelationIsPermanent(rel) ||
				(rel->rd_rel->relpersistence == RELPERSISTENCE_UNLOGGED &&
				 forkNum == INIT_FORKNUM))
				log_smgrcreate(&newrlocator, forkNum);
			RelationCopyStorage(RelationGetSmgr(rel), dstrel, forkNum,
								rel->rd_rel->relpersistence);
		}
	}

	/* 删除旧关系，并关闭新关系 */
	RelationDropStorage(rel);
	smgrclose(dstrel);
}

/*
 * 更改表启用/禁用触发器   
 *
 * 只是转交给 trigger.c。       
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
 * 更改表启用/禁用规则   
 *
 * 只是转交给 rewriteDefine.c。       
 */
static void
ATExecEnableDisableRule(Relation rel, const char *rulename,
						char fires_when, LOCKMODE lockmode)
{
	EnableDisableRule(rel, rulename, fires_when);
}

/*
 * 更改表继承    
 *
 * 向子关系的父列表添加一个父关系。这会验证父关系的   
 * 所有列和检查约束都出现在子关系中，并具有相同的     
 * 数据类型和表达式。     
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
 * 返回新父关系的地址。                
 */
static ObjectAddress
ATExecAddInherit(Relation child_rel, RangeVar *parent, LOCKMODE lockmode)
{
	Relation	parent_rel;
	List	   *children;
	ObjectAddress address;
	const char *trigger_name;

	/*
	 * 此处需要自排他锁。完整说明参见                 
	 * MergeAttributes() 中的类似情形。  
	 */
	parent_rel = table_openrv(parent, ShareUpdateExclusiveLock);

	/*
	 * 必须是父关系和子关系的拥有者 -- 子关系由    
	 * ATPrepCmd/ATSimplePermissions 检查 
	 */
	ATSimplePermissions(AT_AddInherit, parent_rel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/* 永久关系无法继承自临时关系           */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		child_rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation \"%s\"",
						RelationGetRelationName(parent_rel))));

	/* 若父关系是临时表，它必须属于本会话   */
	if (parent_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!parent_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from temporary relation of another session")));

	/* 子关系亦然     */
	if (child_rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!child_rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit to temporary relation of another session")));

	/* 阻止分区表成为继承父表                            */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from partitioned table \"%s\"",
						parent->relname)));

	/* 对分区同样如此   */
	if (parent_rel->rd_rel->relispartition)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot inherit from a partition")));

	/*
	 * 通过检查提议的父表是否继承了子表来防止循环。   
	 * （特别是，这禁止让关系继承自身。）              
	 *
	 * 由于竞争条件，这并非万无一失：在多层继承     
	 * 树中，别人可能并发地建立另一个继承链接，它  
	 * 闭合了循环，但未联结我们已锁定的任何关系。       
	 * 要防止这种情况似乎需要对整个继承树加排他锁，  
	 * 那是比疾病更糟的治疗。无论如何，                     
	 * find_all_inheritors() 都能应付循环，                        
	 * 所以不必太担心。           
	 *
	 * 我们对子表的子表使用能用的最弱锁，即 AccessShareLock 
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
	 * 若 child_rel 拥有带过渡表的行级触发器，目前   
	 * 不允许它成为继承子表。另见 ATExecAttachPartition()   
	 * 和 CreateTrigger() 中的禁令。                         
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(child_rel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming an inheritance child",
						trigger_name, RelationGetRelationName(child_rel)),
				 errdetail("ROW triggers with transition tables are not supported in inheritance hierarchies.")));

	/* 可以创建继承       */
	CreateInheritance(child_rel, parent_rel);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* 在提交前保持父关系上的锁              */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * CreateInheritance
 *		在子表和父表之间创建继承的目录操作部分。        
 *		                         
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

	/* 注意：取 RowExclusiveLock 是因为下面要写 pg_inherits。  */
	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	/*
	 * 检查父列表中是否有重复，并确定已有的最高          
	 * inhseqno；新父表将使用下一个值。另外，若提议的   
	 * 子表是分区，则不允许它已经在继承。         
	 *            
	 *
	 * 注意：我们不拒绝子表已间接继承自父表的情形；   
	 * CREATE TABLE 也不拒绝类似情形。                            
	 */
	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(child_rel)));
	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId,
							  true, NULL, 1, &key);

	/* inhseqno 序列从 1 开始   */
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

	/* 匹配各列，并按需递增 attinhcount          */
	MergeAttributesIntoExisting(child_rel, parent_rel);

	/* 匹配各约束，并按需递增 coninhcount           */
	MergeConstraintsIntoExisting(child_rel, parent_rel);

	/*
	 * 好，看起来有效。创建显示继承关系的目录条目。  
	 */
	StoreCatalogInheritance1(RelationGetRelid(child_rel),
							 RelationGetRelid(parent_rel),
							 inhseqno + 1,
							 catalogRelation,
							 parent_rel->rd_rel->relkind ==
							 RELKIND_PARTITIONED_TABLE);

	/* 已用完 pg_inherits           */
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 * 给定其 pg_constraint 元组，获取检查约束                 
 * 约束表达式的源文本形式        
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
 * 判断两个检查约束在功能上是否等价                   
 *
 * 我们的测试是看它们反向编译后是否得到相同的     
 * 源字符串。这让我们不受诸如父、子关系中属性      
 * 物理列号是否相同之类的问题。                          
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
 * 检查子表中的列与父表中的列是否匹配，并递增它们的   
 * attinhcount。    
 *
 * CreateInheritance 调用   
 *
 * 目前所有父列都必须能在子表中找到。缺少列是            
 * 错误。将来某天我们也许会考虑像 CREATE TABLE              
 * 那样创建新列。然而，这很不得人心 --- 在              
 * 分区表中它是自找麻烦。  
 *
 * 数据类型必须完全匹配。若父列是 NOT NULL，              
 * 则子列也必须是。不过默认值不作比较。        
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

	/* 若 parent_rel 是分区表，则 child_rel 必须是分区         */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	for (parent_attno = 1; parent_attno <= parent_natts; parent_attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc,
													parent_attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/* 忽略父表中已删除的列。     */
		if (attribute->attisdropped)
			continue;

		/* 在子表中找相同的列（按列名匹配）。  */
		tuple = SearchSysCacheCopyAttName(RelationGetRelid(child_rel),
										  attributeName);
		if (HeapTupleIsValid(tuple))
		{
			/* 检查它们的类型、typmod 和排序规则  */
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
			 * 检查子列不会丢弃 NOT NULL 属性。（其他  
			 * 约束在别处检查。）        
			 */
			if (attribute->attnotnull && !childatt->attnotnull)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("column \"%s\" in child table must be marked NOT NULL",
								attributeName)));

			/*
			 * 子列必须是生成列，当且仅当父列是。            
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
			 * 好，递增子列的继承计数。（若稍后失败，  
			 * 此更改只会回滚。）                
			 */
			childatt->attinhcount++;
			if (childatt->attinhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * 对于分区的情况，我们必须强制 attislocal 的值   
			 * 在所有分区中都相同。（注意：分区中只有 
			 * 继承的属性）       
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
 * 检查子表的约束与父表的约束是否匹配，并递增      
 * 它们的 coninhcount。        
 *
 * 在父表中被标为 ONLY 的约束被忽略。           
 *
 * CreateInheritance 调用   
 *
 * 目前父表中的所有约束都必须存在于子表中。将来某天    
 * 也许会考虑像 CREATE TABLE 那样添加约束。      
 *
 * XXX 这是 O(N^2)，对于拥有数百个约束的表                
 * 可能是个问题。不过只要表的约束在 10 个左右，            
 * 就不该有问题。就算 100 个约束也不至于世界末日。     
 *
 * XXX 若你更改此代码，另见 MergeWithExistingConstraint  
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

	/* 若 parent_rel 是分区表，则 child_rel 必须是分区         */
	if (parent_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		child_is_partition = true;

	/* 外层循环扫描父表的约束定义                      */
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

		/* 若父表的约束被标为 NO INHERIT，则它不被继承         */
		if (parent_con->connoinherit)
			continue;

		/* 搜索与该约束匹配的子表约束         */
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

			/* 若子表约束是 "no inherit"，则无法合并         */
			if (child_con->connoinherit)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with non-inherited constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * 若子表约束 "not valid"，则无法与有效的            
			 * 父表约束合并     
			 */
			if (parent_con->convalidated && !child_con->convalidated)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on child table \"%s\"",
								NameStr(child_con->conname),
								RelationGetRelationName(child_rel))));

			/*
			 * 好，递增子表约束的继承数。（若稍后失败，   
			 * 此更改只会回滚。）                
			 */
			child_copy = heap_copytuple(child_tuple);
			child_con = (Form_pg_constraint) GETSTRUCT(child_copy);
			child_con->coninhcount++;
			if (child_con->coninhcount < 0)
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));

			/*
			 * 对于分区，一个继承约束只能               
			 * 被继承一次，因为它无法有多个父表，          
			 * 也永不被视为本地。  
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
 * 更改表无继承    
 *
 * 返回值是不再作为父表的关系的地址。                  
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
	 * 父表上的 AccessShareLock 或许就够了，鉴于 DROP        
	 * TABLE 根本不锁定父表。我们需要某种锁，因为           
	 * 将检查父表的模式。       
	 */
	parent_rel = table_openrv(parent, AccessShareLock);

	/*
	 * 我们不必检查父表的所有权 --- 子表的所有权            
	 * 被推定为足够的权限。      
	 */

	/* 转到 RemoveInheritance() 完成大部分工作          */
	RemoveInheritance(rel, parent_rel, false);

	ObjectAddressSet(address, RelationRelationId,
					 RelationGetRelid(parent_rel));

	/* 在提交前保持父关系上的锁              */
	table_close(parent_rel, NoLock);

	return address;
}

/*
 * MarkInheritDetached
 *
 * 为分区设置 inhdetachpending，供并发模式的          
 * ATExecDetachPartition 使用。同时验证没有其他            
 * 分区已待分离。  
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
	 * 按 inhparent 查找 pg_inherits 条目。（须扫描全部，     
	 * 以验证没有其他分区待分离。）                 
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
			/* 继续查找，确保捕捉到其他待分离的分区 */
		}
	}

	/* 完  */
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
 * RemoveInheritance
 *
 * 从子表的父列表中删除一个父表。这只调整各列的       
 * attinhcount 和 attislocal，并删除 pg_inherit 和                 
 * pg_depend 条目。expect_detached 传递给                          
 *
 * DeleteInheritsTuple（见其文档）。若 attinhcount 降到            
 * 0，则 attislocal 被置为 true。若它回升，attislocal 保持   
 * true，这意味着一旦子表从某个父表移除，其列将     
 * 永远不会被自动删除，这可能令人意外。但至少我们  
 * 绝不会因删除别人没料到的列而造成数据丢失。   
 *
 * 继承约束的 coninhcount 和 conislocal 以完全相同          
 * 的方式调整。   
 *
 * ATExecDropInherit() 和 ATExecDetachPartition() 共用。 
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

	/* 若 parent_rel 是分区表，则 child_rel 必须是分区         */
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
	 * 遍历子表各列，寻找与父关系匹配的列              
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

		/* 已删除或未继承的列忽略  */
		if (att->attisdropped)
			continue;
		if (att->attinhcount <= 0)
			continue;

		if (SearchSysCacheExistsAttName(RelationGetRelid(parent_rel),
										NameStr(att->attname)))
		{
			/* 递减 inhcount，并可能将 islocal 置为 true  */
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
	 * 同样，找到继承的检查约束并解除其继承。为此，   
	 * 我们首先需要父表检查约束名的列表。（我们 
	 * 略施小计，只检查名称是否匹配，并假设          
	 * 表达式也会匹配。）               
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

	/* 现在扫描子表的约束      */
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
			/* 递减 inhcount，并可能将 islocal 置为 true  */
			HeapTuple	copyTuple = heap_copytuple(constraintTuple);
			Form_pg_constraint copy_con = (Form_pg_constraint) GETSTRUCT(copyTuple);

			if (copy_con->coninhcount <= 0) /* 不该发生     */
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
	 * 此继承的 post alter 钩子。由于 object_access_hook             
	 * 不接受多个对象标识符，我们借助 auxiliary_id 参数 
	 * 转发父 oid。      
	 */
	InvokeObjectPostAlterHookArg(InheritsRelationId,
								 RelationGetRelid(child_rel), 0,
								 RelationGetRelid(parent_rel), false);
}

/*
 * 删除由 StoreCatalogInheritance1（CREATE TABLE                    
 * INHERITS/ALTER TABLE INHERIT -- refclassid 将是                        
 * RelationRelationId）或 heap_create_with_catalog（CREATE                 
 * /ALTER TABLE OF -- refclassid 将是 TypeRelationId）。只能遍查    
 * pg_depend。      
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
 * ALTER TABLE OF
 *
 * 把表附加到一个复合类型，就像它是用 CREATE                   
 * TABLE OF 创建的一样。所有 attname、atttypid、                      
 * atttypmod 和 attcollation 都必须匹配。该表                        
 * 不得有继承父表。这些限制确保不会造出怪异配置。        
 *
 * 返回类型的地址。            
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

	/* 验证该类型。 */
	typetuple = typenameType(NULL, ofTypename, NULL);
	check_of_type(typetuple);
	typeform = (Form_pg_type) GETSTRUCT(typetuple);
	typeid = typeform->oid;

	/* 若表有任何继承父表则失败。        */
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
	 * 检查元组描述符的兼容性。与继承不同，我们还       
	 * 要求顺序也匹配。不过 attnotnull 无需匹配。              
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

		/* 取下一个未删除的类型属性。  */
		type_attr = TupleDescAttr(typeTupleDesc, type_attno - 1);
		if (type_attr->attisdropped)
			continue;
		type_attname = NameStr(type_attr->attname);

		/* 取下一个未删除的表属性。      */
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

		/* 比较名称  */
		if (strncmp(table_attname, type_attname, NAMEDATALEN) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table has column \"%s\" where type requires \"%s\"",
							table_attname, type_attname)));

		/* 比较类型  */
		if (table_attr->atttypid != type_attr->atttypid ||
			table_attr->atttypmod != type_attr->atttypmod ||
			table_attr->attcollation != type_attr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("table \"%s\" has different type for column \"%s\"",
							RelationGetRelationName(rel), type_attname)));
	}
	ReleaseTupleDesc(typeTupleDesc);

	/* 表末尾任何剩余列最好都是已删除的。                  */
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

	/* 若表已是类型化表，删除现有依赖。              */
	if (rel->rd_rel->reloftype)
		drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
							   DEPENDENCY_NORMAL);

	/* 记录对新类型的依赖。       */
	tableobj.classId = RelationRelationId;
	tableobj.objectId = relid;
	tableobj.objectSubId = 0;
	typeobj.classId = TypeRelationId;
	typeobj.objectId = typeid;
	typeobj.objectSubId = 0;
	recordDependencyOn(&tableobj, &typeobj, DEPENDENCY_NORMAL);

	/* 更新 pg_class.reloftype */
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
 * 把类型化表与其来源类型分离。只需清除 reloftype         
 * 并删除依赖。    
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
	 * 我们不必检查类型的所有权 --- 表的所有权被        
	 * 推定为足够的权限。类型上也无需任何锁。              
	 */

	drop_parent_dependency(relid, TypeRelationId, rel->rd_rel->reloftype,
						   DEPENDENCY_NORMAL);

	/* 清空 pg_class.reloftype*/
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
 * relation_mark_replica_identity：更新表的复制标识        
 *
 * 仅当 ri_type = REPLICA_IDENTITY_INDEX 时，indexOid                      
 * 合适索引 OID，否则 InvalidOid。 
 *
 * 调用者最好已持有该关系上的排他锁，因为两个         
 * 这样的操作并发运行的后果会很难看。     
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
	 * 检查 relreplident 是否已更改，若是则更新它。 
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
	 * 正确更新各索引的 indisreplident 标志。   
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
			/* 若未设置则设置该位。  */
			if (!pg_index_form->indisreplident)
			{
				dirty = true;
				pg_index_form->indisreplident = true;
			}
		}
		else
		{
			/* 若已设置则清除 */
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
			 * 使表的 relcache 失效，这样在我们提交之后，    
			 * 所有会话会在对该表尝试任何 UPDATE 或 DELETE   
			 * 之前刷新表的复制标识索引。（若在上面      
			 * 更改了该表的 pg_class 行，那么 relcache 失效化  
			 * 已因此进入队列；但我们可能没改。）   
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

	/* 检查该索引是否存在 */
	indexOid = get_relname_relid(stmt->name, rel->rd_rel->relnamespace);
	if (!OidIsValid(indexOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" for table \"%s\" does not exist",
						stmt->name, RelationGetRelationName(rel))));

	indexRel = index_open(indexOid, ShareLock);

	/* 检查该索引是否在我们要改的关系上。     */
	if (indexRel->rd_index == NULL ||
		indexRel->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not an index for table \"%s\"",
						RelationGetRelationName(indexRel),
						RelationGetRelationName(rel))));
	/* AM 必须支持唯一性，且该索引必须确实是唯一的。    */
	if (!indexRel->rd_indam->amcanunique ||
		!indexRel->rd_index->indisunique)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot use non-unique index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* 延迟索引不保证始终唯一。                     */
	if (!indexRel->rd_index->indimmediate)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use non-immediate index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* 表达式索引不受支持。       */
	if (RelationGetIndexExpressions(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use expression index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));
	/* 谓词索引不受支持。         */
	if (RelationGetIndexPredicate(indexRel) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use partial index \"%s\" as replica identity",
						RelationGetRelationName(indexRel))));

	/* 检查索引中可空列。       */
	for (key = 0; key < IndexRelationGetNumberOfKeyAttributes(indexRel); key++)
	{
		int16		attno = indexRel->rd_index->indkey.values[key];
		Form_pg_attribute attr;

		/*
		 * 拒绝任何其他系统列。（往后，我们将一开始就 
		 * 不允许包含这类列的索引，但它们可能存在于      
		 * 较旧的分支中。） 
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

	/* 此索引适合作复制标识。标记它。                  */
	relation_mark_replica_identity(rel, stmt->identity_type, indexOid, true);

	index_close(indexRel, NoLock);
}

/*
 * 更改表启用/禁用行级安全           
 */
static void
ATExecSetRowSecurity(Relation rel, bool rls)
{
	Relation	pg_class;
	Oid			relid;
	HeapTuple	tuple;

	relid = RelationGetRelid(rel);

	/* 拉取该关系的记录并更新它            */
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
 * 更改表强制/不强制行级安全        
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

	/* 提取当前选项          */
	datum = SysCacheGetAttr(FOREIGNTABLEREL,
							tuple,
							Anum_pg_foreign_table_ftoptions,
							&isnull);
	if (isnull)
		datum = PointerGetDatum(NULL);

	/* 转换这些选项    */
	datum = transformGenericOptions(ForeignTableRelationId,
									datum,
									options,
									fdw->fdwvalidator);

	if (PointerIsValid(DatumGetPointer(datum)))
		repl_val[Anum_pg_foreign_table_ftoptions - 1] = datum;
	else
		repl_null[Anum_pg_foreign_table_ftoptions - 1] = true;

	repl_repl[Anum_pg_foreign_table_ftoptions - 1] = true;

	/* 一切看起来没问题 - 更新元组  */

	tuple = heap_modify_tuple(tuple, RelationGetDescr(ftrel),
							  repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(ftrel, &tuple->t_self, tuple);

	/*
	 * 使 relcache 失效，让所有会话刷新任何可能               
	 * 依赖旧选项的缓存计划。    
	 */
	CacheInvalidateRelcache(rel);

	InvokeObjectPostAlterHook(ForeignTableRelationId,
							  RelationGetRelid(rel), 0);

	table_close(ftrel, RowExclusiveLock);

	heap_freetuple(tuple);
}

/*
 * 更改表更改列设置压缩方法    
 *
 * 返回值是被修改列的地址                 
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

	/* 复制此缓存条目，以便下面涂改它       */
	tuple = SearchSysCacheCopyAttName(RelationGetRelid(rel), column);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, RelationGetRelationName(rel))));

	/* 阻止它们修改系统属性                */
	atttableform = (Form_pg_attribute) GETSTRUCT(tuple);
	attnum = atttableform->attnum;
	if (attnum <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot alter system column \"%s\"", column)));

	/*
	 * 检查列类型是否可压缩，然后获取属性           
	 * 压缩方法代码     
	 */
	cmethod = GetAttributeCompression(atttableform->atttypid, compression);

	/* 更新 pg_attribute 条目*/
	atttableform->attcompression = cmethod;
	CatalogTupleUpdate(attrel, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(RelationRelationId,
							  RelationGetRelid(rel),
							  attnum);

	/*
	 * 把更改也应用到索引（仅适用于简单索引列，       
	 * 与 index.c ConstructTupleDescriptor() 一致）。      
	 */
	SetIndexStorageProperties(rel, attrel, attnum,
							  false, 0,
							  true, cmethod,
							  lockmode);

	heap_freetuple(tuple);

	table_close(attrel, RowExclusiveLock);

	/* 使更改可见      */
	CommandCounterIncrement();

	ObjectAddressSubSet(address, RelationRelationId,
						RelationGetRelid(rel), attnum);
	return address;
}


/*
 * SET LOGGED/UNLOGGED 的准备阶段      
 *
 * 这验证我们没有试图修改临时表。此外，还会      
 * 检查现有的外键约束，以避免最终出现永久表        
 * 引用不记录 WAL 表的情况。          
 *
 * 若操作是空操作则返回 false（此时跳过这些           
 * 检查），否则返回 true。    
 */
static bool
ATPrepChangePersistence(Relation rel, bool toLogged)
{
	Relation	pg_constraint;
	HeapTuple	tuple;
	SysScanDesc scan;
	ScanKeyData skey[1];

	/*
	 * 不允许更改临时表的状态。还要确认能否                
	 * 什么都不做就蒙混过去；这类情况下也无需          
	 * 下面的检查。   
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
				/* 无事可做  */
				return false;
			break;
		case RELPERSISTENCE_UNLOGGED:
			if (!toLogged)
				/* 无事可做  */
				return false;
			break;
	}

	/*
	 * 改为 UNLOGGED 时检查该表不属于任何出版物，因为    
	 * UNLOGGED 表无法被发布。                  
	 */
	if (!toLogged &&
		GetRelationPublications(RelationGetRelid(rel)) != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot change table \"%s\" to unlogged because it is part of a publication",
						RelationGetRelationName(rel)),
				 errdetail("Unlogged relations cannot be replicated.")));

	/*
	 * 检查现有的外键约束，以维护永久表                     
	 * 不引用不记录 WAL 表的不变量。自引用                
	 * 外键可以放心忽略。        
	 */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);

	/*
	 * 若改为永久表则扫描 conrelid，否则扫描 confrelid。  
	 * 这也决定是否存在可用索引。  
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

			/* 即所用扫描键的另一端              */
			foreignrelid = toLogged ? con->confrelid : con->conrelid;

			/* 若为自引用则忽略   */
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

	/* 若为被某列拥有的序列，禁止单独移动它。*/
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

	/* 获取并锁定模式 OID，并检查其权限。  */
	newrv = makeRangeVar(stmt->newschema, RelationGetRelationName(rel), -1);
	nspOid = RangeVarGetAndCheckCreationNamespace(newrv, NoLock, NULL);

	/* 切换命名空间的公共检查     */
	CheckSetNamespace(oldNspOid, nspOid);

	objsMoved = new_object_addresses();
	AlterTableNamespaceInternal(rel, oldNspOid, nspOid, objsMoved);
	free_object_addresses(objsMoved);

	ObjectAddressSet(myself, RelationRelationId, relid);

	if (oldschema)
		*oldschema = oldNspOid;

	/* 关闭关系，但保持锁到提交  */
	relation_close(rel, NoLock);

	return myself;
}

/*
 * 把表或物化视图迁移到另一命名空间的核心部分：       
 * 除了移动关系本身，其依赖对象也被迁移                    
 * 到新模式   
 */
void
AlterTableNamespaceInternal(Relation rel, Oid oldNspOid, Oid nspOid,
							ObjectAddresses *objsMoved)
{
	Relation	classRel;

	Assert(objsMoved != NULL);

	/* 好，修改 pg_class 行和 pg_depend 条目   */
	classRel = table_open(RelationRelationId, RowExclusiveLock);

	AlterRelationNamespaceInternal(classRel, RelationGetRelid(rel), oldNspOid,
								   nspOid, true, objsMoved);

	/* 若表有行类型，也一并修正        */
	if (OidIsValid(rel->rd_rel->reltype))
		AlterTypeNamespaceInternal(rel->rd_rel->reltype,
								   nspOid, false, false, objsMoved);

	/* 修正其他依赖项     */
	AlterIndexNamespaces(classRel, rel, oldNspOid, nspOid, objsMoved);
	AlterSeqNamespaces(classRel, rel, oldNspOid, nspOid,
					   objsMoved, AccessExclusiveLock);
	AlterConstraintNamespaces(RelationGetRelid(rel), oldNspOid, nspOid,
							  false, objsMoved);

	table_close(classRel, RowExclusiveLock);
}

/*
 * 把关系迁移到另一命名空间的核心部分：修正 pg_class   
 * 条目，以及 pg_depend 条目（若有）。调用者必须  
 * 已打开并写锁定 pg_class   
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

	/* relkind=c 无关系锁，故用 LOCKTAG_TUPLE  */
	classTup = SearchSysCacheLockedCopy1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(classTup))
		elog(ERROR, "cache lookup failed for relation %u", relOid);
	classForm = (Form_pg_class) GETSTRUCT(classTup);

	Assert(classForm->relnamespace == oldNspOid);

	thisobj.classId = RelationRelationId;
	thisobj.objectId = relOid;
	thisobj.objectSubId = 0;

	/*
	 * 若对象已被移动，则不再移动。若已在正确的       
	 * 位置，则不移动，但仍会触发对象访问                 
	 * 钩子。   
	 */
	already_done = object_address_present(&thisobj, objsMoved);
	if (!already_done && oldNspOid != newNspOid)
	{
		ItemPointerData otid = classTup->t_self;

		/* 检查重名（比唯一索引失败更友好）                   */
		if (get_relname_relid(NameStr(classForm->relname),
							  newNspOid) != InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("relation \"%s\" already exists in schema \"%s\"",
							NameStr(classForm->relname),
							get_namespace_name(newNspOid))));

		/* classTup 是副本，因此可以涂改  */
		classForm->relnamespace = newNspOid;

		CatalogTupleUpdate(classRel, &otid, classTup);
		UnlockTuple(classRel, &otid, InplaceUpdateTupleLock);


		/* 若调用者要求则更新对模式的依赖 */
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
 * 把指定关系的所有索引移到另一命名空间。        
 *
 * 注意：我们假定调用者已做充分的权限检查，        
 * 并且对拥有关系持有合适的锁。                     
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
		 * 注意：目前索引对命名空间没有自己的依赖，故   
		 * 无需执行 changeDependencyFor()。pg_type 中亦无             
		 * 行类型。                
		 *
		 * XXX 这个 objsMoved 测试可能是多余的 -- 关系到每个   
		 * 索引难道不是只有一条依赖链接？ 
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
 * 把指定关系的所有标识列和 SERIAL 列序列移到另一命名空间。  
 *           
 *
 * 注意：我们假定调用者已做充分的权限检查，        
 * 并且对拥有关系持有合适的锁。                     
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
	 * SERIAL 序列是那些对表的某一列有自动依赖的        
	 * 序列（我们并不在乎确切是*哪一*列）。   
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
	/* 我们不指定 refobjsubid      */

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);
		Relation	seqRel;

		/* 跳过除对列的自动依赖以外的依赖             */
		if (depForm->refobjsubid == 0 ||
			depForm->classid != RelationRelationId ||
			depForm->objsubid != 0 ||
			!(depForm->deptype == DEPENDENCY_AUTO || depForm->deptype == DEPENDENCY_INTERNAL))
			continue;

		/* 使用 relation_open 以防它是索引      */
		seqRel = relation_open(depForm->objid, lockmode);

		/* 跳过非序列关系       */
		if (RelationGetForm(seqRel)->relkind != RELKIND_SEQUENCE)
		{
			/* 无需保留锁          */
			relation_close(seqRel, lockmode);
			continue;
		}

		/* 修正 pg_class 和 pg_depend 条目   */
		AlterRelationNamespaceInternal(classRel, depForm->objid,
									   oldNspOid, newNspOid,
									   true, objsMoved);

		/*
		 * 序列过去在 pg_type 中有条目，但现在没有了。       
		 * 若将来恢复这一点，需把 pg_type 条目也移到        
		 * 新命名空间（用 AlterTypeNamespaceInternal）   
		 */
		Assert(RelationGetForm(seqRel)->reltype == InvalidOid);

		/* 现在可以关闭它。保持锁直到事务结束。       */
		relation_close(seqRel, NoLock);
	}

	systable_endscan(scan);

	relation_close(depRel, AccessShareLock);
}


/*
 * 此代码支持   
 *	CREATE TEMP TABLE ... ON COMMIT { DROP | PRESERVE ROWS | DELETE ROWS }
 *
 * 由于我们只对 TEMP 表支持此功能，把状态记在             
 * 后端本地的数据结构中即可。     
 */

/*
 * 注册新创建关系的 ON COMMIT 动作。         
 */
void
register_on_commit_action(Oid relid, OnCommitAction action)
{
	OnCommitItem *oc;
	MemoryContext oldcxt;

	/*
	 * 除非确有需要采取的 ON COMMIT 动作，否则                  
	 * 无需注册关系。  
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
	 * 我们在这里使用 lcons()，这样 ON COMMIT 动作就            
	 * 按注册的相反顺序处理。可能并非必需，          
	 * 但合理  
	 */
	on_commits = lcons(oc, on_commits);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * 删除关系时注销其 ON COMMIT 动作。               
 *
 * 实际上，我们只是把 OnCommitItem 条目标记为                    
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
 * 执行 ON COMMIT 动作   
 *
 * 它在真正提交之前被调用，因为可能遇到错误。     
 *                     
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

		/* 若已在本事务中删除则忽略条目   */
		if (oc->deleting_subid != InvalidSubTransactionId)
			continue;

		switch (oc->oncommit)
		{
			case ONCOMMIT_NOOP:
			case ONCOMMIT_PRESERVE_ROWS:
				/* 什么都不做（实际上不应该有这种条目） */
				break;
			case ONCOMMIT_DELETE_ROWS:

				/*
				 * 若本事务未访问过任何临时关系，则 
				 * 可跳过对 ON COMMIT DELETE ROWS 表的清空，     
				 * 因为它们必定仍为空。      
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
	 * 先清空关系再删除，这样各关系之间的依赖          
	 * 会在它们被处理之后才移除。这样做可能是          
	 * 浪费，因为被清空的关系很可能因其父表                 
	 * 被删而反正会被删掉，但这让代码更健壮，            
	 * 因为清空时无需重新检查关系                             
	 * 是否存在。           
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
		 * 对象删除可能涉及 TOAST 表访问（清理被         
		 * TOAST 的目录条目），故确保持有有效快照。    
		 */
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * 由于这是自动删除，而不是用户直接发起的，       
		 * 我们传入 PERFORM_DELETION_INTERNAL 标志。        
		 */
		performMultipleDeletions(targetObjects, DROP_CASCADE,
								 PERFORM_DELETION_INTERNAL | PERFORM_DELETION_QUIETLY);

		PopActiveSnapshot();

#ifdef USE_ASSERT_CHECKING

		/*
		 * 注意表删除会调用 remove_on_commit_action，因此         
		 * 该条目将标记为已删除。  
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
 * ON COMMIT 管理的提交后或中止后清理。          
 *
 * 我们所做的只是移除不再需要的 OnCommitItem        
 *
 * 条目。提交时，移除本事务中被删除的条目；            
 * 中止时，移除本事务中创建的条目。           
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
			/* cur_item 必须被移除 */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item 必须被保留   */
			oc->creating_subid = InvalidSubTransactionId;
			oc->deleting_subid = InvalidSubTransactionId;
		}
	}
}

/*
 * ON COMMIT 管理的子提交后或子中止后清理。          
 *
 * 子中止时，可以立即移除本子事务中创建的             
 * 条目。子提交时，只是把本子事务中标记的            
 * 条目重新标注为父事务的责任。               
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
			/* cur_item 必须被移除 */
			on_commits = foreach_delete_current(on_commits, cur_item);
			pfree(oc);
		}
		else
		{
			/* cur_item 必须被保留   */
			if (oc->creating_subid == mySubid)
				oc->creating_subid = parentSubid;
			if (oc->deleting_subid == mySubid)
				oc->deleting_subid = isCommit ? parentSubid : InvalidSubTransactionId;
		}
	}
}

/*
 * 这是 RangeVarGetRelidExtended() 的回调。它                        
 * 只允许在以下情况下锁定关系：(1) 它是普通表、        
 * 分区表、物化视图或 TOAST 表，且 (2) 当前用户               
 * 是所有者（或超级用户）。这满足 CLUSTER、             
 * REINDEX TABLE 和 REFRESH MATERIALIZED VIEW 的权限检查               
 * 之需；故公开 
 */
void
RangeVarCallbackOwnsTable(const RangeVar *relation,
						  Oid relId, Oid oldRelId, void *arg)
{
	char		relkind;

	/*  若未找到该关系，则无事可做。  */
	if (!OidIsValid(relId))
		return;

	/*
	 *  若该关系确实存在，检查它是否为索引。但请注意， 
	 *  该关系可能在我们完成名称查找之后已被删除。     
	 *  若是如此，则无事可做。                   
	 */
	relkind = get_rel_relkind(relId);
	if (!relkind)
		return;
	if (relkind != RELKIND_RELATION && relkind != RELKIND_TOASTVALUE &&
		relkind != RELKIND_MATVIEW && relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table or materialized view", relation->relname)));

	/*  检查权限     */
	if (!object_ownercheck(RelationRelationId, relId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relId)), relation->relname);
}

/*
 *  TRUNCATE 处理的 RangeVarGetRelidExtended() 回调函数。 
 */
static void
RangeVarCallbackForTruncate(const RangeVar *relation,
							Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/*  若未找到该关系，则无事可做。  */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/*  不应发生     */
		elog(ERROR, "cache lookup failed for relation %u", relId);

	truncate_check_rel(relId, (Form_pg_class) GETSTRUCT(tuple));
	truncate_check_perms(relId, (Form_pg_class) GETSTRUCT(tuple));

	ReleaseSysCache(tuple);
}

/*
 *  RangeVarGetRelidExtended() 的回调。检查当前用户是否         
 *  是关系所有者或超级用户。   
 */
void
RangeVarCallbackOwnsRelation(const RangeVar *relation,
							 Oid relId, Oid oldRelId, void *arg)
{
	HeapTuple	tuple;

	/*  若未找到该关系，则无事可做。  */
	if (!OidIsValid(relId))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relId));
	if (!HeapTupleIsValid(tuple))	/*  不应发生     */
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
 *  通用 RangeVarGetRelid 回调：重命名、设置模式、修改表  
 *  处理。 
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
		return;					/*  已被并发删除  */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	relkind = classform->relkind;

	/*  须为所有者。*/
	if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)), rv->relname);

	/*  除非明确允许，不允许修改系统表。        */
	if (!allowSystemTableMods && IsSystemClass(relid, classform))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						rv->relname)));

	/*
	 *  从语句的语法分析树中提取所指定的关系类型。  
	 *
	 *  另外，对 ALTER .. RENAME 而言，需检查权限：用户    
	 *  必须仍拥有命名空间 CREATE 权限。  
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
		reltype = OBJECT_TABLE; /*  安抚编译器 */
	}

	/*
	 *  为兼容旧版，允许将 ALTER TABLE 用于大多数其他类型  
	 *  的关系（组合类型除外）。我们也允许 ALTER INDEX 在  
	 *  RENAME 情况下同样灵活，其他情况则不允许。       
	 *  否则，用户必须为所涉关系选用正确命令形式。   
	 *                                   
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
	 *  不允许对组合类型使用 ALTER TABLE，希望用户改用 ALTER 
	 *  TYPE。      
	 */
	if (reltype != OBJECT_TYPE && relkind == RELKIND_COMPOSITE_TYPE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is a composite type", rv->relname),
		/* translator: %s is an SQL ALTER command */
				 errhint("Use %s instead.",
						 "ALTER TYPE")));

	/*
	 *  不允许对无法移至其他模式的关系使用 ALTER TABLE ..   
	 *  SET SCHEMA，例如索引和 TOAST 表。              
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
 *  转换分区键中存在的任何表达式。        
 *
 * 返回转换后的 PartitionSpec。 
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

	/*  检查该策略的有效列数。         */
	if (partspec->strategy == PARTITION_STRATEGY_LIST &&
		list_length(partspec->partParams) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("cannot use \"list\" partition strategy with more than one column")));

	/*
	 *  创建虚拟 ParseState，将目标关系插入为其中唯一的  
	 *  范围表条目。transformExpr 需要 ParseState。     
	 */
	pstate = make_parsestate(NULL);
	nsitem = addRangeTableEntryForRelation(pstate, rel, AccessShareLock,
										   NULL, false, true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*  处理任意分区表达式。        */
	foreach(l, partspec->partParams)
	{
		PartitionElem *pelem = lfirst_node(PartitionElem, l);

		if (pelem->expr)
		{
			/*  复制一份，以免改写输入。  */
			pelem = copyObject(pelem);

			/*  现在对该表达式做语法分析转换。*/
			pelem->expr = transformExpr(pstate, pelem->expr,
										EXPR_KIND_PARTITION_EXPRESSION);

			/*  还需修正其排序规则。   */
			assign_expr_collations(pstate, pelem->expr);
		}

		newspec->partParams = lappend(newspec->partParams, pelem);
	}

	return newspec;
}

/*
 *  从 PartitionElem 列表中计算每个分区列的信息。          
 *  PartitionElem 中的表达式必须已完成语法分析。     
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
			/*  简单的属性引用     */
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
			 *  生成列行不通：其在 BEFORE 触发器之后计算， 
			 *  但分区路由在所有触发器之前就已执行。     
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
			/*  表达式 */
			Node	   *expr = pelem->expr;
			char		partattname[16];
			Bitmapset  *expr_attrs = NULL;
			int			i;

			Assert(expr != NULL);
			atttype = exprType(expr);
			attcollation = exprCollation(expr);

			/*
			 *  表达式必须为可存储类型（RECORD 即不行）。  
			 *  此测试与判断表列是否为安全类型的测试相同  
			 * （因此我们无需检查非表达式的情形）。    
			 *       
			 */
			snprintf(partattname, sizeof(partattname), "%d", attn + 1);
			CheckAttributeType(partattname,
							   atttype, attcollation,
							   NIL, CHKATYPE_IS_PARTKEY);

			/*
			 *  去除顶层 COLLATE 子句。这确保 "x COLLATE y" 与     
			 *  "(x COLLATE y)" 同等看待。        
			 */
			while (IsA(expr, CollateExpr))
				expr = (Node *) ((CollateExpr *) expr)->arg;

			/*
			 *  检查分区键表达式里的所有列。                  
			 *  若存在整行引用，则检查分区表的全部列。    
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
				 *  不允许引用系统列，因为分区路由无法   
				 *  进行：待到需要路由时，这些值还未知。 
				 *                                   
				 */
				if (attno < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("partition key expressions cannot contain system column references")));

				/*
				 *  生成列不行：在 BEFORE 触发器后计算，   
				 *  但分区路由在所有触发器之前执行。        
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
				 * 用户写"(column)"或"(column COLLATE something)"。 
				 *  一律按简单属性对待。       
				 */
				partattrs[attn] = ((Var *) expr)->varattno;
			}
			else
			{
				partattrs[attn] = 0;	/*  将该列标记为表达式   */
				*partexprs = lappend(*partexprs, expr);

				/*
				 *  基于分区表达式的 EXPR_KIND_，              
				 *  transformPartitionSpec() 应已拒绝子查询、       
				 *  聚合、窗口函数和 SRF。            
				 */

				/*
				 *  检查可变性前需先预处理表达式，原因见  
				 * contain_mutable_functions_after_planning      
				 * 。但我们会自行调用 expression_planner 而不是   
				 *  用那个函数：若常量折叠把表达式化成   
				 *  常量，我们想知道，以便报错。      
				 *  与 contain_mutable_functions_after_planning 一样，   
				 *  假定        
				 *
				 *  expression_planner 不会改写输入，故不影响     
				 *  我们在上面保存的 partexprs 条目。          
				 *                                                 
				 */
				expr = (Node *) expression_planner((Expr *) expr);

				/*
				 *  分区表达式无法包含易变函数，            
				 *  因为只要分区边界不变，每一行都必须     
				 *  映射到同一个分区。                           
				 *           
				 */
				if (contain_mutable_functions(expr))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("functions in partition key expression must be marked IMMUTABLE")));

				/*
				 *  分区表达式为常量虽不算完全*错误*，但    
				 *  拒绝这样的分区键似乎更好。              
				 */
				if (IsA(expr, Const))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("cannot use constant expression as partition key")));
			}
		}

		/*
		 *  有排序覆盖则应用。   
		 */
		if (pelem->collation)
			attcollation = get_collation_oid(pelem->collation, false);

		/*
		 *  排序规则只应存在于可排序类型。此处预期失  
		 *  败有两种：（1）COLLATE 用于不可排序类型，或    
		 *  （2）分区表达式有未定的排序规则。不过我们不 
		 *  防将此处编码成完备的一致性检查。             
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
		 *  确定合适的操作符类。列表与范围分区使用  
		 *  btree 操作符类；哈希分区则使用 hash 操作符类。   
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
 *		 scanrel 的现有约束是否蕴含该分区约束？           
 *
 *  "现有约束"包括其检查约束与列级 NOT NULL 约束。     
 *  partConstraint 描述该分区约束，为隐式 AND 形式。          
 *                      
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
				 *  argisrow=false 对组合列同样正确，因为     
				 *  此时 attnotnull 并非 SQL 规范的 IS NOT NULL    
				 *  测试，而只是 IS DISTINCT FROM NULL 测试。  
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
 *		 scanrel 的现有约束是否蕴含给定约束？          
 *
 *  testConstraint 是待验证的约束。provenConstraint 是       
 *  调用方提供的条件列表，本函数可假定其为真。 
 *  provenConstraint 与 testConstraint 都必须为隐式        
 *  AND 形式，只能包含不可变子句，且只能包含     
 *  varno = 1 的 Var。            
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
		 *  若该约束尚未完整验证，则此处必须忽略它。     
		 *         
		 */
		if (!constr->check[i].ccvalid)
			continue;

		cexpr = stringToNode(constr->check[i].ccbin);

		/*
		 *  每个表达式都需常量简化与规范化，   
		 *  这必不可少：我们将把它与经过同样处理的分      
		 *  区约束表达式相比较，否则可能无法检出有效   
		 *  的匹配。                             
		 */
		cexpr = eval_const_expressions(NULL, cexpr);
		cexpr = (Node *) canonicalize_qual((Expr *) cexpr, true);

		existConstraint = list_concat(existConstraint,
									  make_ands_implicit((Expr *) cexpr));
	}

	/*
	 *  尝试证明。由于我们比较的是 CHECK 约束，需要用   
	 *  弱蕴含：假定 existConstraint 不为假，并尝试证明 
	 *  testConstraint 亦是如此。                        
	 *
	 *  注意 predicate_implied_by 假定其第一个实参为已知    
	 *  知不可变。对 NOT NULL 约束与分区约束而言这一        
	 *  点恒应成立，此处不再测。 
	 */
	return predicate_implied_by(testConstraint, existConstraint, true);
}

/*
 * QueuePartitionConstraintValidation
 *
 *  向 wqueue 添加一个条目，让给定的分区约束在第 3 阶段  
 *  验证，针对给定关系及全部子关系。     
 *
 *  我们首先验证给定约束是否被既有关系约束所蕴含； 
 *  若是，就无需扫描表来进行验证，因此这种情况下 
 *  不必加入队列。                
 */
static void
QueuePartitionConstraintValidation(List **wqueue, Relation scanrel,
								   List *partConstraint,
								   bool validate_default)
{
	/*
	 *  根据该表现有的约束，判断我们是否可以跳过扫描   
	 *  该表。                  
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
	 *  约束被证明不足。普通关系则立即把验证项   
	 *  排入队列；分区表则递归处理每个分区。             
	 *           
	 */
	if (scanrel->rd_rel->relkind == RELKIND_RELATION)
	{
		AlteredTableInfo *tab;

		/*  取得队列条目。   */
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
			 *  这是防止死锁所需的最低级别的锁。     
			 */
			part_rel = table_open(partdesc->oids[i], AccessExclusiveLock);

			/*
			 *  调整 scanrel 约束以匹配本分区属性号。     
			 *                               
			 */
			thisPartConstraint =
				map_partition_varattnos(partConstraint, 1,
										part_rel, scanrel);

			QueuePartitionConstraintValidation(wqueue, part_rel,
											   thisPartConstraint,
											   validate_default);
			table_close(part_rel, NoLock);	/*  保持锁至提交。*/
		}
	}
}

/*
 * ALTER TABLE <name> ATTACH PARTITION <partition-name> FOR VALUES
 *
 *  返回新附加的分区的地址。              
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
	 *  若存在默认分区，我们必须将其锁定，因为附加新  
	 *  分区会改变它的分区约束。              
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(rel, true));
	if (OidIsValid(defaultPartOid))
		LockRelationOid(defaultPartOid, AccessExclusiveLock);

	attachrel = table_openrv(cmd->name, AccessExclusiveLock);

	/*
	 *  XXX 我认为此时最好也对外键引用的所有表获取锁。  
	 *                           
	 */

	/*
	 *  必须是父表和源表两者的所有者 -- 父表已由           
	 *  ATPrepCmd 的 ATSimplePermissions   
	 */
	ATSimplePermissions(AT_AttachPartition, attachrel, ATT_TABLE | ATT_FOREIGN_TABLE);

	/*  一个分区只能有一个父表。*/
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
	 *  被附加的表不应已是继承关系的一部分；要么作为   
	 *  子表...         
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

	/*  ...或作为父表（但已分区的情况除外）。          */
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
	 *  检查 rel 是否为 attachrel 的分区以防止循环。（特别 
	 *  是，这不允许把 rel 变成其自身的分区。）      
	 *
	 *  做法：若 attachrel 是分区表，则检查 rel 是否属于   
	 *  其分区列表。我们希望避免再次构造这个列表，因此 
	 *  我们对所有分区请求最强级别的锁。之所以需要     
	 *  最强的锁：因为若发现被附加的表（或其叶分区）  
	 *  可能包含违反分区约束的行，便可能决定扫描。      
	 *  若该表拥有能阻止这类行的约束（按定义它存在于   
	 *  所有分区中），则无需扫描该表和它的分区。但 
	 *  我们无法冒险先取较弱的锁，而等到需要时才取 
	 *  更强的锁。                                                     
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

	/*  若父表是永久表，其所有分区也必须如此。    */
	if (rel->rd_rel->relpersistence != RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a temporary relation as partition of permanent relation \"%s\"",
						RelationGetRelationName(rel))));

	/*  临时父表无法拥有本身非临时的分区。          */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		attachrel->rd_rel->relpersistence != RELPERSISTENCE_TEMP)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach a permanent relation as partition of temporary relation \"%s\"",
						RelationGetRelationName(rel))));

	/*  若父表是临时表，它必须属于本会话。  */
	if (rel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!rel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach as partition of temporary relation of another session")));

	/*  分区亦然。        */
	if (attachrel->rd_rel->relpersistence == RELPERSISTENCE_TEMP &&
		!attachrel->rd_islocaltemp)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot attach temporary relation of another session as partition")));

	/*  检查 attachrel 中是否有父表没有的列。                   */
	tupleDesc = RelationGetDescr(attachrel);
	natts = tupleDesc->natts;
	for (attno = 1; attno <= natts; attno++)
	{
		Form_pg_attribute attribute = TupleDescAttr(tupleDesc, attno - 1);
		char	   *attributeName = NameStr(attribute->attname);

		/*  已删除列  */
		if (attribute->attisdropped)
			continue;

		/*  尝试在父表中找到该列（按列名匹配）。    */
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
	 *  若 child_rel 拥有带转换表的行级触发器，我们  
	 *  目前不允许它成为分区。另请参见 ATExecAddInherit()     
	 *  与 CreateTrigger() 中的禁令。      
	 */
	trigger_name = FindTriggerIncompatibleWithInheritance(attachrel->trigdesc);
	if (trigger_name != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("trigger \"%s\" prevents table \"%s\" from becoming a partition",
						trigger_name, RelationGetRelationName(attachrel)),
				 errdetail("ROW triggers with transition tables are not supported on partitions.")));

	/*
	 *  检查新分区的边界是否有效且不与父表任何现有      
	 *  分区重叠 -- 注意它出错时并不返回。                    
	 *       
	 */
	check_new_partition_bound(RelationGetRelationName(attachrel), rel,
							  cmd->bound, pstate);

	/*  可以创建继承。其余检查在那里进行             */
	CreateInheritance(attachrel, rel);

	/*  更新 pg_class 条目。 */
	StorePartitionBound(attachrel, rel, cmd->bound);

	/*  确保分区中存在正确的一组索引。                 */
	AttachPartitionEnsureIndexes(rel, attachrel);

	/*  及触发器*/
	CloneRowTriggersToPartition(rel, attachrel);

	/*
	 *  克隆外键约束。被调用方负责为第 3 阶段约束验证 
	 *  做好准备工作。              
	 */
	CloneForeignKeyConstraints(wqueue, rel, attachrel);

	/*
	 *  根据分区边界说明生成分区约束。                       
	 *  若父表本身是分区，则须连其约束也包含      
	 *  进来。         
	 */
	partBoundConstraint = get_qual_from_partbound(rel, cmd->bound);
	partConstraint = list_concat(partBoundConstraint,
								 RelationGetPartitionQual(rel));

	/*  若没有需要验证的约束，则跳过验证。     */
	if (partConstraint)
	{
		/*
		 *  与检查约束类似，将分区限定条件经过常量简  
		 *  化处理。我们跳过 canonicalize_qual，因为分区    
		 *  限定条件本应是规范形式。               
		 */
		partConstraint =
			(List *) eval_const_expressions(NULL,
											(Node *) partConstraint);

		/*  XXX 看来肯定不对   */
		partConstraint = list_make1(make_ands_explicit(partConstraint));

		/*
		 *  调整生成的约束，使其匹配此分区的属性编号。   
		 *         
		 */
		partConstraint = map_partition_varattnos(partConstraint, 1, attachrel,
												 rel);

		/*  针对被附加的表验证分区约束。                      */
		QueuePartitionConstraintValidation(wqueue, attachrel, partConstraint,
										   false);
	}

	/*
	 *  若我们附加的不是默认分区而默认分区又存在，     
	 *  那么该分区的分区约束就会改变，因此也要向工       
	 *  作队列加入一个条目来验证它。（被附加的分区       
	 *  正是默认分区时不得这样做；上面已经做过了！）  
	 *               
	 */
	if (OidIsValid(defaultPartOid))
	{
		Relation	defaultrel;
		List	   *defPartConstraint;

		Assert(!cmd->bound->is_default);

		/*  我们已持有默认分区上的锁           */
		defaultrel = table_open(defaultPartOid, NoLock);
		defPartConstraint =
			get_proposed_default_constraint(partBoundConstraint);

		/*
		 *  将约束表达式 Var 从 rel attno 映射到 defaultrel。 
		 *              
		 */
		defPartConstraint =
			map_partition_varattnos(defPartConstraint,
									1, defaultrel, rel);
		QueuePartitionConstraintValidation(wqueue, defaultrel,
										   defPartConstraint, true);

		/*  锁保持到提交为止。*/
		table_close(defaultrel, NoLock);
	}

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(attachrel));

	/*
	 *  若刚附加的分区自身也是分区表，则同样要使其   
	 *  所有后代分区的 relcache 失效，以确保其          
	 *  rd_partcheck 表达式树得以重建；各分区已在本函数      
	 *  开头被锁定。            
	 */
	if (attachrel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		ListCell   *l;

		foreach(l, attachrel_children)
		{
			CacheInvalidateRelcacheByRelid(lfirst_oid(l));
		}
	}

	/*  锁保持到提交。     */
	table_close(attachrel, NoLock);

	return address;
}

/*
 * AttachPartitionEnsureIndexes
 *		 ATExecAttachPartition 子例程：创建/匹配索引      
 *
 *  在 ALTER TABLE / ATTACH PARTITION 期间执行分区表的索引规则： 
 *  每个分区对于分区表上的每个索引都必须有与之           
 *  附加索引。  
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

	/*  构建所有现有索引及其 IndexInfo 的数组       */
	i = 0;
	foreach(cell, attachRelIdxs)
	{
		Oid			cldIdxId = lfirst_oid(cell);

		attachrelIdxRels[i] = index_open(cldIdxId, AccessShareLock);
		attachInfos[i] = BuildIndexInfo(attachrelIdxRels[i]);
		i++;
	}

	/*
	 *  若我们附加的是外部表，则只要有任何一个索引是   
	 *  约束索引就必须失败；否则这里无事可做。请在开   
	 *  始工作之前先做此检查，以免先构建了几个非唯   
	 *  一索引后才碰上唯一索引，白费功夫。 
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
	 *  对于分区表上每个索引，在即将成为分区的表中   
	 *  找匹配的索引；若无则创建一个。   
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
		 *  忽略分区表中除分区索引之外的其他索引。    
		 *         
		 */
		if (idxRel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX)
		{
			index_close(idxRel, AccessShareLock);
			continue;
		}

		/*  构建一个 indexinfo，用于与现有索引比较      */
		info = BuildIndexInfo(idxRel);
		attmap = build_attrmap_by_name(RelationGetDescr(attachrel),
									   RelationGetDescr(rel),
									   false);
		constraintOid = get_relation_idx_constraint_oid(RelationGetRelid(rel), idx);

		/*
		 *  扫描即将成为分区表中现有的索引列表，并把找  
		 * 到的第一个匹配、有效且未附加的索引标记为 
		 *  父索引的分区。找到即完成。                   
		 */
		for (i = 0; i < list_length(attachRelIdxs); i++)
		{
			Oid			cldIdxId = RelationGetRelid(attachrelIdxRels[i]);
			Oid			cldConstrOid = InvalidOid;

			/*  此索引已有父索引？若有则不可用      */
			if (attachrelIdxRels[i]->rd_rel->relispartition)
				continue;

			/*  若此索引无效，则不可用     */
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
				 *  若父表中此索引因约束而创建，              
				 *  则子表也需有约束，故要找一下。若无这类 
				 *  约束，该索引便不合用，继续找。        
				 *                                   
				 */
				if (OidIsValid(constraintOid))
				{
					cldConstrOid =
						get_relation_idx_constraint_oid(RelationGetRelid(attachrel),
														cldIdxId);
					/*  没戏 */
					if (!OidIsValid(cldConstrOid))
						continue;
				}

				/*  找到*/
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
		 *  若在即将成为分区的表中没找到合适索引，则    
		 * 。 
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
	/*  清理。*/
	for (i = 0; i < list_length(attachRelIdxs); i++)
		index_close(attachrelIdxRels[i], AccessShareLock);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(cxt);
}

/*
 * CloneRowTriggersToPartition
 *		 ATExecAttachPartition/DefineRelation 的子例程，创建      
 *		分区行触发器。 
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
		 *  忽略语句级触发器；它们不会被克隆。  
		 */
		if (!TRIGGER_FOR_ROW(trigForm->tgtype))
			continue;

		/*
		 *  不要克隆内部触发器，因为约束克隆代码会做。  
		 *      
		 */
		if (trigForm->tgisinternal)
			continue;

		/*
		 *  若发现意外的触发器类型则报错。 
		 */
		if (!TRIGGER_FOR_BEFORE(trigForm->tgtype) &&
			!TRIGGER_FOR_AFTER(trigForm->tgtype))
			elog(ERROR, "unexpected trigger \"%s\" found",
				 NameStr(trigForm->tgname));

		/*  为 CREATE TRIGGER 使用短命上下文  */
		oldcxt = MemoryContextSwitchTo(perTupCxt);

		/*
		 *  若有 WHEN 子句，则为其生成一个适合该分区的      
		 *  "cooked" 版本。           
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
		 *  若有列列表，则把它转换为列名列表。注意我们  
		 *  不需要以任何方式映射这个列表…… 
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

		/*  重建触发器参数列表。     */
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
		trigStmt->funcname = NULL;	/*  单独传入     */
		trigStmt->args = trigargs;
		trigStmt->row = true;
		trigStmt->timing = trigForm->tgtype & TRIGGER_TYPE_TIMING_MASK;
		trigStmt->events = trigForm->tgtype & TRIGGER_TYPE_EVENT_MASK;
		trigStmt->columns = cols;
		trigStmt->whenClause = NULL;	/*  单独传入     */
		trigStmt->transitionRels = NIL; /*  目前不支持         */
		trigStmt->deferrable = trigForm->tgdeferrable;
		trigStmt->initdeferred = trigForm->tginitdeferred;
		trigStmt->constrrel = NULL; /*  单独传入     */

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
 * 更改表分离分区       
 *
 *  返回不再是 rel 分区的那个关系的地址。                  
 *
 *  若请求并发模式，我们将分两个事务运行。其副     
 *  作用是该命令无法在多部分 ALTER TABLE 中运行。目  
 *  前这一点由语法强制执行。     
 *
 *  并发策略是：先改分区的 pg_inherits 行以表明该 
 *  分区已分离，锁住分区禁写入，并提交事务； 
 *  从那时起，任何请求分区描述符的人必须忽略这种 
 *  分区。第二个事务中，我们等待所有可能将        
 *  该分区看作附加状态的事务全部结束，再删除  
 *  其余分区元数据（pg_inherits 和                            
 *  pg_class.relpartbounds）。                                       
 *                                                   
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
	 *  我们必须锁定默认分区，因为分离该分区会改变它 
	 *  的分区约束。                  
	 */
	partdesc = RelationGetPartitionDesc(rel, true);
	defaultPartOid = get_default_oid_from_partdesc(partdesc);
	if (OidIsValid(defaultPartOid))
	{
		/*
		 *  存在默认分区时不支持并发分离。问题在于 
		 *  默认分区约束会改变。此外还有定义上的  
		 *  问题：正在插入且属于被分离分区的元组该怎么  
		 *  办？把它们放到被分离的分区上是错的，因为分  
		 *  离完成后会变得"丢失"，但在我们修改其分区      
		 *  约束之前，也无法把它们放进默认分区。      
		 *                                                                    
		 *                                                
		 *
		 *  我认为，如果我们在提交第一个事务之前就实施 
		 *  约束变更，这个问题是可以解决的。但那样锁就   
		 *  只得保持为 AEL，并会使并发查询计划受阻，因此 
		 *  那样改反而更糟。                               
		 */
		if (concurrent)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot detach partitions concurrently when a default partition exists")));
		LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	/*
	 *  并发模式下，分区在第一个事务中被锁定为             
	 *  share-update-exclusive；这允许并发事务对其做 DML。     
	 *                            
	 */
	partRel = table_openrv(name, concurrent ? ShareUpdateExclusiveLock :
						   AccessExclusiveLock);

	/*
	 *  检查继承条件，然后（在非并发模式下）删除         
	 *  pg_inherits 行，或仅设置 inhdetachpending 标志。 
	 */
	if (!concurrent)
		RemoveInheritance(partRel, rel, false);
	else
		MarkInheritDetached(partRel, rel);

	/*
	 *  确保分离后外键仍然成立。这会保持引用表上的  
	 *  锁，从而阻止并发事务加入我们看不见的行。要让    
	 *  这一点在并发模式下成立，关键是第一个事务一提   
	 *  交，分区对 RI 查询就表现为不再处于附加状态。    
	 *                                                             
	 */
	ATDetachCheckNoForeignKeyRefs(partRel);

	/*
	 *  并发模式必须做更多工作：首先向分区添加一个与 
	 *  分区约束匹配的约束。然后结束现有事务，并在新    
	 *  事务中等待所有进程追上我们已做的目录更新；      
	 *  到那时即可完成操作。                                  
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
		 *  对哈希以外的策略，向被分离的分区添加一个取 
		 *  代分区约束的约束。对于哈希我们无法这样做， 
		 *  因约束会引用分区表 OID，之后可能出问题。  
		 *                                                        
		 */
		if (partdesc->boundinfo->strategy != PARTITION_STRATEGY_HASH)
			DetachAddConstraintIfNeeded(wqueue, partRel);

		/*
		 *  现在已近完成；仅剩的痕迹是 pg_inherits      
		 *  元组和该分区的 relpartbounds。在删除它们之前，需 
		 *  待这些事务结束。                                          
		 *                              
		 */

		/*
		 *  记住关系 OID 以便稍后重新获取；也记住关系名， 
		 *  以防其间有对象被删除时用于报错。          
		 */
		partrelid = RelationGetRelid(partRel);
		parentrelid = RelationGetRelid(rel);
		parentrelname = MemoryContextStrdup(PortalContext,
											RelationGetRelationName(rel));
		partrelname = MemoryContextStrdup(PortalContext,
										  RelationGetRelationName(partRel));

		/*  使父表的 relcache 条目失效 -- 必须在关闭之前       */
		CacheInvalidateRelcache(rel);

		table_close(partRel, NoLock);
		table_close(rel, NoLock);
		tab->rel = NULL;

		/*  使更新后的目录条目可见 */
		PopActiveSnapshot();
		CommitTransactionCommand();

		StartTransactionCommand();

		/*
		 *  现在等待。这确保所有计划时包含该分区   
		 *  的查询都在我们删除其余目录条目前完成。不过 
		 *  我们既不需要也不希望获取这个锁 --             
		 *  那会阻塞之后的查询。                
		 *
		 *  至于等待分区本身的锁，无需为此操心，因为    
		 *  下面就会获取 AccessExclusiveLock。                        
		 */
		SET_LOCKTAG_RELATION(tag, MyDatabaseId, parentrelid);
		WaitForLockersMultiple(list_make1(&tag), AccessExclusiveLock, false);

		/*
		 *  现在重新获取两个关系的锁。注意它们可能在此   
		 *  期间已被删除，所以需要小心。  
		 */
		rel = try_relation_open(parentrelid, ShareUpdateExclusiveLock);
		partRel = try_relation_open(partrelid, AccessExclusiveLock);

		/*  若关系已不存在，说明出了坏事；退出            */
		if (rel == NULL)
		{
			if (partRel != NULL)	/*  不应发生    */
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
	 *  分离分区可能涉及 TOAST 表访问，因此确保我们        
	 *  有有效快照。   
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	/*  执行分离的最后一步   */
	DetachPartitionFinalize(rel, partRel, concurrent, defaultPartOid);

	PopActiveSnapshot();

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partRel));

	/*  锁保持到提交。     */
	table_close(partRel, NoLock);

	return address;
}

/*
 *  ALTER TABLE .. DETACH 第二部分  
 *
 *  把它独立出来，是为了在并发算法的第二个事务失 
 *  败（崩溃或中止）时也能独立运行。              
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
		 *  现在可以删除 pg_inherits 行了。（非并发情形已做  
		 *  过。）             
		 */
		RemoveInheritance(partRel, rel, true);
	}

	/*  删除创建/附加时克隆的所有触发器。     */
	DropClonedTriggersFromPartition(RelationGetRelid(partRel));

	/*
	 *  分离所有被继承的外键。这包括创建额外的         
	 *  动作触发器。        
	 */
	fks = copyObject(RelationGetFKeyList(partRel));
	if (fks != NIL)
		trigrel = table_open(TriggerRelationId, RowExclusiveLock);

	/*
	 *  被分离的分区可能拥有一个引用分区表的外键。      
	 *  这种情况下，该分区会有多个 pg_constraint 行：         
	 *  一个指向分区表本身，其余指向它的各个分区。   
	 *  这里只考虑最顶层的那一个；子约束须原样保留，  
	 *  因为从概念上讲，它们并非来自我们的父分区表，    
	 *  而是来自这个分区本身。                                  
	 *                                                   
	 *
	 *  具体做法是：第一遍扫描 FK 数组时收集所有约束      
	 *  的 OID，然后在下面的循环中跳过其父约束已列入      
	 *  这里的约束。     
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
		 *  只考虑被继承的外键，且仅当父约束不在列表  
		 *  中时。         
		 */
		if (conform->contype != CONSTRAINT_FOREIGN ||
			!OidIsValid(conform->conparentid) ||
			list_member_oid(fkoids, conform->conparentid))
		{
			ReleaseSysCache(contup);
			continue;
		}

		/*
		 *  此表上的约束必须标记为不再是父约束的子      
		 *  约束，其检查触发器也是如此。       
		 */
		ConstraintSetParentConstraint(fk->conoid, InvalidOid, InvalidOid);

		/*
		 *  另外，查找分区中与被分离约束相对应的"检查"触 
		 *  发器，并把它们从父触发器上分离。                  
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
		 *  最后，用 addFkRecurseReferenced 在被引用表上创建    
		 *  动作触发器；该函数需要一些繁琐的准备（因此   
		 *  放在单独块中）。另外若该表是分区表，该函数   
		 *  还会递归为每个分区创建 pg_constraint 行与动作  
		 *  触发器。                      
		 *
		 *  注意此处无需调用 addFkConstraint()，因为          
		 *  pg_constraint 行已存在。   
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

			/*  创建一个后面通用的合成节点     */
			fkconstraint = makeNode(Constraint);
			fkconstraint->contype = CONSTRAINT_FOREIGN;
			fkconstraint->conname = pstrdup(NameStr(conform->conname));
			fkconstraint->deferrable = conform->condeferrable;
			fkconstraint->initdeferred = conform->condeferred;
			fkconstraint->skip_validation = true;
			fkconstraint->initially_valid = true;
			/*  此处省略若干无关字段      */
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

			/*  设置 colnames，用于生成约束名              */
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
			table_close(refdRel, NoLock);	/*  锁保持至事务结束  */
		}

		ReleaseSysCache(contup);
	}
	list_free_deep(fks);
	if (trigrel)
		table_close(trigrel, RowExclusiveLock);

	/*
	 *  位于较大约束被引用侧的子约束都必须删除。  
	 *  该分区已不再是该约束键空间的一部分。                
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

	/*  现在可以分离索引 */
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
		 *  若该索引关联有约束，则同样将其分离。注意： 
		 *  分区中的约束索引有可能是一个非约束索引的子   
		 *  索引，因此要核实父索引是否真的带有约束。    
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

	/* 更新 pg_class 元组*/
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(partRel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(partRel));
	Assert(((Form_pg_class) GETSTRUCT(tuple))->relispartition);

	/* 清空 relpartbound 并复位 relispartition*/
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
		 *  若被分离的关系就是默认分区本身，则将其从  
		 *  父表的 pg_partitioned_table 条目中删除。     
		 *
		 *  否则我们必须使默认分区的 relcache 条目失效，   
		 *  就像在 StorePartitionBound 中那样：其分区约束       
		 *  依赖其他分区的分区约束。  
		 */
		if (RelationGetRelid(partRel) == defaultPartOid)
			update_default_partition_oid(RelationGetRelid(rel), InvalidOid);
		else
			CacheInvalidateRelcacheByRelid(defaultPartOid);
	}

	/*
	 *  使父表的 relcache 失效，让该分区不再包含在其     
	 *  分区描述符中。               
	 */
	CacheInvalidateRelcache(rel);

	/*
	 *  若刚分离的分区自身是分区表，则同样要使其所   
	 *  有后代分区的 relcache 失效，确保其 rd_partcheck   
	 *  表达式树得以重建；在此之前必须锁定这些分区，   
	 *  锁模式与调用方锁定 partRel 时相同。                     
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
 * 更改表 ... 分离分区 ... FINALIZE      
 *
 *  用于先前未运行完毕的 DETACH PARTITION 命令；      
 *  它会完成分离过程。                     
 */
static ObjectAddress
ATExecDetachPartitionFinalize(Relation rel, RangeVar *name)
{
	Relation	partRel;
	ObjectAddress address;
	Snapshot	snap = GetActiveSnapshot();

	partRel = table_openrv(name, AccessExclusiveLock);

	/*
	 *  等到现有快照全部消失。若 DETACH PARTITION           
	 *  CONCURRENTLY 的第二个事务被取消，这一点很重         
	 *  要：用户可能立即运行 DETACH FINALIZE，而不真正         
	 *  等待现有事务。在这些查询全部完成前，我们绝不 
	 *  能完成分离（否则它们看到的目录视图不一致）。   
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
 *		 ATExecDetachPartition 的子过程。创建一个取代分    
 *		 区约束的约束；但若已有能蕴含所需约束的约  
 *		 束，则避免创建重复的约束。                       
 *		           
 */
static void
DetachAddConstraintIfNeeded(List **wqueue, Relation partRel)
{
	List	   *constraintExpr;

	constraintExpr = RelationGetPartitionQual(partRel);
	constraintExpr = (List *) eval_const_expressions(NULL, (Node *) constraintExpr);

	/*
	 *  若所需约束已被某个现有约束蕴含，则不要添加新    
	 *  约束            
	 */
	if (!PartConstraintImpliedByRelConstraint(partRel, constraintExpr))
	{
		AlteredTableInfo *tab;
		Constraint *n;

		tab = ATGetQueueEntry(wqueue, partRel);

		/*  在分区上添加等价于分区约束的约束                   */
		n = makeNode(Constraint);
		n->contype = CONSTR_CHECK;
		n->conname = NULL;
		n->location = -1;
		n->is_no_inherit = false;
		n->raw_expr = NULL;
		n->cooked_expr = nodeToString(make_ands_explicit(constraintExpr));
		n->initially_valid = true;
		n->skip_validation = true;
		/*  这是重新添加，因为它名义上已存在*/
		ATAddCheckConstraint(wqueue, tab, partRel, n,
							 true, false, true, ShareUpdateExclusiveLock);
	}
}

/*
 * DropClonedTriggersFromPartition
 *		 用于 ATExecDetachPartition 的子例程：删除分区在         
 *		 以分区身份创建或被附加时克隆的所有触发器。     
 *		 撤销 CloneRowTriggersToPartition 的效果。 
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
	 *  扫描 pg_trigger 查找此 rel 的全部触发器。 
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

		/*  忽略未被克隆的触发器     */
		if (!OidIsValid(pg_trigger->tgparentid))
			continue;

		/*
		 *  忽略作为外键实现对象的内部触发器，因为外键   
		 *  本身被分离时它们也会被分离。               
		 *                
		 */
		if (OidIsValid(pg_trigger->tgconstrrelid))
			continue;

		/*
		 *  这么做很难看，但确有必要：移除该触发器上的  
		 *  依赖标记以便删除。      
		 */
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										TriggerRelationId,
										DEPENDENCY_PARTITION_PRI);
		deleteDependencyRecordsForClass(TriggerRelationId, pg_trigger->oid,
										RelationRelationId,
										DEPENDENCY_PARTITION_SEC);

		/*  记住该触发器，稍后删除       */
		ObjectAddressSet(trig, TriggerRelationId, pg_trigger->oid);
		add_exact_object_address(&trig, objects);
	}

	/*  使依赖删除对下面的删除操作可见            */
	CommandCounterIncrement();
	performMultipleDeletions(objects, DROP_RESTRICT, PERFORM_DELETION_INTERNAL);

	/*  完 */
	free_object_addresses(objects);
	systable_endscan(scan);
	table_close(tgrel, RowExclusiveLock);
}

/*
 *  获取索引上的锁之前，先取其所属表上的同样锁。   
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
	 *  若我们此前锁过其他堆表，而当前查找的名字已      
	 *  不再指向该关系上的索引，则释放这个已无用的      
	 *  锁。XXX 也许应该在确认索引确实不属于同一           
	 *  关系*之后*再释放……              
	 */
	if (relOid != oldRelOid && OidIsValid(state->partitionOid))
	{
		UnlockRelationOid(state->partitionOid, AccessShareLock);
		state->partitionOid = InvalidOid;
	}

	/*  未找到关系，无需锁定或做权限检查。                 */
	if (!OidIsValid(relOid))
		return;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relOid));
	if (!HeapTupleIsValid(tuple))
		return;					/*  已被并发删除，无事可做     */
	classform = (Form_pg_class) GETSTRUCT(tuple);
	if (classform->relkind != RELKIND_PARTITIONED_INDEX &&
		classform->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("\"%s\" is not an index", rv->relname)));
	ReleaseSysCache(tuple);

	/*
	 *  只需查看堆表的元组描述符，对其持访问共享锁     
	 *  （阻止任何 DDL）即可。         
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
	 *  我们需要获取索引 name 上的锁才能修改它，同时    
	 *  还要读其所属表的元组描述符 -- 所以两者都要锁。  
	 *  为避免死锁，先锁表再锁索引。此外还需要查看分   
	 *  区的父表，因此也要锁定父表。                        
	 *                                 
	 */
	state.partitionOid = InvalidOid;
	state.parentTblOid = parentIdx->rd_index->indrelid;
	state.lockedParentTbl = false;
	partIdxId =
		RangeVarGetRelidExtended(name, AccessExclusiveLock, 0,
								 RangeVarCallbackForAttachIndex,
								 (void *) &state);
	/*  不在？ */
	if (!OidIsValid(partIdxId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("index \"%s\" does not exist", name->relname)));

	/*  无死锁风险：RangeVarGetRelidExtended 已取得锁             */
	partIdx = relation_open(partIdxId, AccessExclusiveLock);

	/*  两表均已持锁，因此这样是安全的：      */
	parentTbl = relation_open(parentIdx->rd_index->indrelid, AccessShareLock);
	partTbl = relation_open(partIdx->rd_index->indrelid, NoLock);

	ObjectAddressSet(address, RelationRelationId, RelationGetRelid(partIdx));

	/*  已处于目标状态则静默不做             */
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
		 *  若该分区已附加了索引，则拒绝该操作。    
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

		/*  确保其索引的是另一索引所属表的分区        */
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

		/*  确保两个索引兼容         */
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
		 *  若父索引带有约束，确保子索引也带有。          
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

		/*  通过，执行  */
		IndexSetParentIndex(partIdx, RelationGetRelid(parentIdx));
		if (OidIsValid(constraintOid))
			ConstraintSetParentConstraint(cldConstrId, constraintOid,
										  RelationGetRelid(partTbl));

		free_attrmap(attmap);

		validatePartitionedIndex(parentIdx, parentTbl);
	}

	relation_close(parentTbl, AccessShareLock);
	/*  保持这些锁直到提交 */
	relation_close(partTbl, NoLock);
	relation_close(partIdx, NoLock);

	return address;
}

/*
 *  验证给定分区是否已存在附加到给定分区索引的     
 *  索引。若存在则报错。                        
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
 *  验证分区表上某父索引所附加的分区索引集合是         
 *  否完整。若完整，则把父索引标记为有效。              
 *
 *  每次附加分区索引后都应调用本函数。          
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
	 *  扫描该父索引的 pg_inherits。每找到一个有效索          
	 *  引就计数（并逐一核验其 pg_index 条目），若达到 
	 *  预期总数，即可把该父索引标记为有效。  
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

	/*  pg_inherits 已扫完*/
	systable_endscan(scan);
	table_close(inheritsRel, AccessShareLock);

	/*
	 *  若找到的继承索引数与分区表的分区数相同，即  
	 *  可更新 pg_index 置 indisvalid 为有效。           
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
	 *  若该索引本身又是更大索引的分区，验证它可能     
	 *  使其父索引也随之有效。尝试一下。      
	 */
	if (updated && partedIdx->rd_rel->relispartition)
	{
		Oid			parentIdxId,
					parentTblId;
		Relation	parentIdx,
					parentTbl;

		/*  确保能看到刚完成的验证结果    */
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
 *  返回引用给定关系、且被标记为带有父约束的约   
 *  束的 OID 列表。                          
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
	 *  若没有索引，或没有可被外键引用的列，可免扫描    
	 *      
	 */
	if (RelationGetIndexList(partition) == NIL ||
		bms_is_empty(RelationGetIndexAttrBitmap(partition,
												INDEX_ATTR_BITMAP_KEY)))
		return NIL;

	/*  搜索引用此表的约束                  */
	pg_constraint = table_open(ConstraintRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_constraint_confrelid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(RelationGetRelid(partition)));
	ScanKeyInit(&key[1],
				Anum_pg_constraint_contype, BTEqualStrategyNumber,
				F_CHAREQ, CharGetDatum(CONSTRAINT_FOREIGN));

	/*  XXX 这是顺序扫描，没有可用索引           */
	scan = systable_beginscan(pg_constraint, InvalidOid, true, NULL, 2, key);
	while ((tuple = systable_getnext(scan)) != NULL)
	{
		Form_pg_constraint constrForm = (Form_pg_constraint) GETSTRUCT(tuple);

		/*
		 *  只需处理属于更大约束的那些约束。                
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
 *  分离分区期间，验证指向该分区表的外键不会因此  
 *  失效。若存在被引用的值则报错。                        
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

		/*  阻止向引用表写数据，直到提交为止。         */
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
		/*  剩余字段无需填写。        */

		RI_PartitionRemove_Check(&trig, rel, partition);

		ReleaseSysCache(tuple);

		table_close(rel, NoLock);
	}
}

/*
 *  将列压缩说明解析为压缩方法。                    
 */
static char
GetAttributeCompression(Oid atttypid, char *compression)
{
	char		cmethod;

	if (compression == NULL || strcmp(compression, "default") == 0)
		return InvalidCompressionMethod;

	/*
	 *  要指定非默认方法，列数据类型必须可 TOAST。          
	 *  注意：这与列的 attstorage 设置是否允许压缩无关， 
	 *  有意让 attstorage 与 attcompression 独立。但对  
	 *  不可 TOAST 的类型，attstorage 无法设置为允许       
	 *  的值。                                                           
	 *
	 *  我们实际上不必强制这一点，因为即使 attcompression   
	 *  为非默认也不会有坏结果；它永远不会被查阅。但  
	 *  对注定无用的属性设置尝试报错，似乎更          
	 *  友好。                  
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
 *  解析列存储说明。           
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
	 *  安全检查：除非列数据类型支持 TOAST，否则不允许    
	 *  存储。     
	 */
	if (!(cstorage == TYPSTORAGE_PLAIN || TypeIsToastable(atttypid)))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("column data type %s can only have storage PLAIN",
						format_type_be(atttypid))));

	return cstorage;
}
