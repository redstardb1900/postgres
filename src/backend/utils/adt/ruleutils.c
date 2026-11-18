/*-------------------------------------------------------------------------
 *
 * ruleutils.c
 *	把已存储的表达式/查询树还原为
 *	源文本
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ruleutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "common/keywords.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/varlena.h"
#include "utils/xml.h"

/* ----------
 * 美化格式化常量
 * ----------
 */

/* 缩进计数 */
#define PRETTYINDENT_STD		8
#define PRETTYINDENT_JOIN		4
#define PRETTYINDENT_VAR		4

#define PRETTYINDENT_LIMIT		40	/*换行限制*/

/* 美化标志 */
#define PRETTYFLAG_PAREN		0x0001
#define PRETTYFLAG_INDENT		0x0002
#define PRETTYFLAG_SCHEMA		0x0004

/* 将 "bool pretty" 选项标准转换为详细标志 */
#define GET_PRETTY_FLAGS(pretty) \
	((pretty) ? (PRETTYFLAG_PAREN | PRETTYFLAG_INDENT | PRETTYFLAG_SCHEMA) \
	 : PRETTYFLAG_INDENT)

/* 美化打印换行的默认行长度：0 表示总是换行 */
#define WRAP_COLUMN_DEFAULT		0

/* 测试是否需要美化动作的宏 */
#define PRETTY_PAREN(context)	((context)->prettyFlags & PRETTYFLAG_PAREN)
#define PRETTY_INDENT(context)	((context)->prettyFlags & PRETTYFLAG_INDENT)
#define PRETTY_SCHEMA(context)	((context)->prettyFlags & PRETTYFLAG_SCHEMA)


/* ----------
 * Local data types
 * ----------
 */

/* 调用递归查询树显示例程所需的上下文信息 */
typedef struct
{
	StringInfo	buf;			/* 待追加的输出缓冲区*/
	List	   *namespaces;		/* deparse_namespace 节点列表 */
	TupleDesc	resultDesc;		/* 视图顶层时为该视图的 tupdesc */
	List	   *targetList;		/* 当前查询层的 SELECT 目标列表 */
	List	   *windowClause;	/* 当前查询层的 WINDOW 子句 */
	int			prettyFlags;	/* 启用美化打印函数 */
	int			wrapColumn;		/* 最大行长度，-1 表示无限制*/
	int			indentLevel;	/* 美化打印的当前缩进级别 */
	bool		varprefix;		/* true 时打印 Var 的前缀 */
	bool		colNamesVisible;	/* 是否关心输出列的名称？ */
	bool		inGroupBy;		/*反解析 GROUP BY 子句？*/
	bool		varInOrderBy;	/* 反解析 ORDER BY 简单 Var？*/
	Bitmapset  *appendparents;	/* 若非空，则把这些 relid 的子 Var
								 * 映射回父关系 */
} deparse_context;

/*
 * 子树外围的每一层查询上下文都需要一层 Var 命名空间。
 * varlevelsup=N 的 Var 引用当前上下文 namespaces 列表中的
 * 第 N 项（从 0 起算）。
 *
 * rtable 是来自 Query 或 PlannedStmt 的实际 RTE 列表。
 * rtable_names 保存每个 RTE 使用的别名（可为 C 字
 * 符串，或对无名 RTE（无名连接）为 NULL）。
 * rtable_columns 保存每个 RTE 要使用的列别名。
 *
 * subplans 是 SubPlan 与 CTE 的 Plan 树列表（仅在
 * PlannedStmt 情形）。
 * ctes 是 CommonTableExpr 节点列表（仅 Query 情形使用）。
 * appendrels 若非空（仅在 PlannedStmt 情形使用），是一个
 * 以子 relid 为索引的 AppendRelInfo 节点数组。我们用它来
 * 把子表 Var 映射到其继承父关系。
 *
 * 某些情况下，我们需要让合并的 JOIN USING 列的名字在
 * 整个查询中唯一，而不只限单个 RTE。此时 unique_using
 * 为真，且 using_names 是 C 字符串列表，存放已分配给
 * USING 列名。
 *
 * 对 plan 树进行反解析时，deparse_namespace 列表中始终
 * 只有一项（因为 plan 树不含 varlevelsup > 0 的 Var）。
 * 我们保存作为待反解析表达式直接父节点的 Plan
 * 节点，以及该 Plan 的祖先列表。此外，我们还
 * 保存其外部与内部子 plan 节点及其目标列表，
 * 以及当前 plan 节点可能含有 INDEX_VAR Var 时的
 * 索引目标列表。（这些字段本可从当前 Plan 节点即时
 * 导出，但将它们设为独立字段在记法上似乎更清晰。）
 *
 */
typedef struct
{
	List	   *rtable;			/* RangeTblEntry 节点列表 */
	List	   *rtable_names;	/* 与 RTE 并行的名字列表 */
	List	   *rtable_columns; /* 与 RTE 平行的 deparse_columns 列表 */
	List	   *subplans;		/* SubPlan 的 Plan 树列表 */
	List	   *ctes;			/* CommonTableExpr 节点列表 */
	AppendRelInfo **appendrels; /* AppendRelInfo 节点数组或 NULL */
	/* 用于列别名分配的工作区： */
	bool		unique_using;	/* 是否要让 USING 名全局唯一 */
	List	   *using_names;	/* 已分配给 USING 列的名字列表 */
	/* 其余字段仅在反解析 plan 树时使用： */
	Plan	   *plan;			/* 当前表达式的直接父节点 */
	List	   *ancestors;		/* plan 的祖先 */
	Plan	   *outer_plan;		/*外部子节点，无则为 NULL*/
	Plan	   *inner_plan;		/*内部子节点，无则为 NULL*/
	List	   *outer_tlist;	/* OUTER_VAR Var 的参照 */
	List	   *inner_tlist;	/* INNER_VAR Var 的参照 */
	List	   *index_tlist;	/* INDEX_VAR Var 的参照 */
	/* 表示函数签名的特殊命名空间： */
	char	   *funcname;
	int			numargs;
	char	  **argnames;
} deparse_namespace;

/*
 * 每个关系的列别名相关信息。
 *
 * 选择别名异常复杂，因为规则/视图的底层表可能在查询
 * 解析之后增加、删除或重命名过列。我们仍必须以可
 * 重新载入、且产生与之前相同结果的形式打印该规则/
 * 视图。
 *
 * 对于查询中用到的每个 RTE，我们必须分配在该 RTE 内
 * 唯一的列别名。SQL 并不要求原始查询做到这一点，但
 * 由于 *-展开 等因素，我们需要能唯一引用反解析
 * 查询中的每一列。只要所有列引用都带限定，每个 RTE
 * 内唯一便已足够。
 *
 * 然而，对于未命名的连接 RTE，我们无法保证其列名的唯
 * 一性，因为它们只是从输入 RTE 继承列名，而无法在
 * 连接层重命名它们。多数时候这不是问题，因为我们不
 * 需要按列本身引用连接输出列；可改为引用输入列。
 * 但对合并的 JOIN USING 列而言这种做法会失效，因此当
 * 未命名连接中出现这种列时，必须让该列别名在整个
 * 查询范围内全局唯一，以确保能无歧义地引用它。
 *
 *
 * 另一个问题是，JOIN USING 子句要求被合并的列在两个
 * 输入 RTE 中有相同别名，且这些 RTE 及其子关系中没
 * 有其他列与 USING 名冲突。为处理这一点，我们在查询
 * 的连接树递归遍历中做 USING 列别名分配。当沿
 * 带 USING 的 JOIN 向下走时，我们把 USING 列名预先指派
 * 给子列，覆盖列别名分配的其他规则。我们还给每个 RTE
 * 标记上包含它的连接所选的全部 USING 列名，这样以后
 * 分配其他列的别名时就能避开冲突。
 *
 *
 * 另一个问题是，如果 JOIN 的输入表在查询被解析之后增
 * 加或删除过列，我们必须为该连接生成与当前输入列集
 * 相匹配的列别名列表 —— 否则左输入列数变化会使
 * 别名与右输入列的匹配错位。因此可打印列别名列表
 * 的位置不一定与 JOIN 的 varattno 一一对应，所以需要一
 * 个单独的 new_colnames[] 数组用于打印。
 *
 */
typedef struct
{
	/*
	 * colnames 是数组，存放查询被解析时就已存在的列所用
	 * 列别名。已删除的列为 NULL 项。
	 * 该数组可直接用 varattno 索引来取得 Var 的名字。
	 *
	 * 非 NULL 项保证在 RTE 内唯一，*除非* 用于未命名的
	 * JOIN RTE。那种情况下只是从两个输入 RTE 汇集名字。
	 *
	 *
	 * 在 set_using_names() 的递归下降过程中，对子 RTE 列名
	 * 的强制指派表示为预置子 RTE 的 colnames 数组对应
	 * 元素。所以该阶段 NULL 项只意味着尚未预指派名字，
	 * 而不一定是该列已被删除。
	 *
	 */
	int			num_cols;		/* colnames[] 数组的长度 */
	char	  **colnames;		/* C 字符串与 NULL 的数组*/

	/*
	 * new_colnames 是数组，存放若按基表当前定义重新解析
	 * 查询时会存在的列所用的别名。这才是要作为该
	 * RTE 列别名列表打印的内容。此数组不含已删列，但
	 * 会包含最初解析后新增的列。因此其索引与当前
	 * varattno 值基本没有关系。如上所述，除非这是用于
	 * 未命名的 JOIN RTE，否则各项都是唯一的。（在这类
	 * RTE 中实际从不打印此数组，但仍必须计算它，以备
	 * 日后计算上层连接列名之用。）与之并行的 is_new_col
	 * 数组标记这些列中哪些是最初解析后新增的。
	 * is_new_col 为假的项必须与非 NULL 的 colnames 项
	 * 一一对应。
	 *
	 */
	int			num_new_cols;	/* new_colnames[] 数组的长度 */
	char	  **new_colnames;	/* C 字符串数组 */
	bool	   *is_new_col;		/* 布尔标志数组 */

	/* 该标志指示我们是否真的要打印列别名列表 */
	bool		printaliases;

	/* 该列表有在此 RTE 之上连接中用作 USING 名的名字 */
	List	   *parentUsing;	/* 指派给父合并列的名字 */

	/*
	 * 如果此结构用于 JOIN RTE，我们会在 set_using_names()
	 * 阶段填充这些字段，以描述它与其子 RTE 的关系。
	 *
	 * leftattnos/rightattnos 是数组，连接每个现存输出列
	 * 各占一项（可用连接 varattno 索引）。若简单引用
	 * 的是左子关系的列，则 leftattnos[i] 是子 RTE 的 attno，
	 * 且 rightattnos[i] 为零；右子关系的列则相反。但对
	 * JOIN USING/NATURAL JOIN 产生的合并列，leftattnos[i]
	 * 与 rightattnos[i] 都非零。注意简单引用可能指向已删除
	 * 的子 RTE 列；没关系，该列不可能被查询用到。
	 *
	 *
	 * 若是 JOIN USING，usingNames 保存为合并列选定的别名
	 * （若为达成唯一性而修改过名字，它们可能与原始
	 * USING 列表不同）。
	 */
	int			leftrti;		/* 左子关系的范围表索引 */
	int			rightrti;		/* 右子关系的范围表索引 */
	int		   *leftattnos;		/* 连接列的左子 varattno，或为 0 */
	int		   *rightattnos;	/* 连接列的右子 varattno，或为 0 */
	List	   *usingNames;		/* 指派给合并列的名字 */
} deparse_columns;

/* 此宏类似于 rt_fetch()，但用于 deparse_columns 结构 */
#define deparse_columns_fetch(rangetable_index, dpns) \
	((deparse_columns *) list_nth((dpns)->rtable_columns, (rangetable_index)-1))

/*
 * set_rtable_names 的哈希表条目
 */
typedef struct
{
	char		name[NAMEDATALEN];	/* 哈希键 --- 必须居首 */
	int			counter;		/* 该名字迄今的最大附加数 */
} NameHashEntry;

/* resolve_special_varno() 的回调函数签名 */
typedef void (*rsv_callback) (Node *node, deparse_context *context,
							  void *callback_arg);


/* ----------
 * Global data
 * ----------
 */
static SPIPlanPtr plan_getrulebyoid = NULL;
static const char *query_getrulebyoid = "SELECT * FROM pg_catalog.pg_rewrite WHERE oid = $1";
static SPIPlanPtr plan_getviewrule = NULL;
static const char *query_getviewrule = "SELECT * FROM pg_catalog.pg_rewrite WHERE ev_class = $1 AND rulename = $2";

/* GUC 参数 */
bool		quote_all_identifiers = false;


/* ----------
 * 局部函数
 *
 * 这些函数过去大多用固定大小的缓冲区来构建结果。
 * 现在它们接受已初始化的 StringInfo 对象作参数，
 * 并把文本输出追加到其内容中。
 * ----------
 */
static char *deparse_expression_pretty(Node *expr, List *dpcontext,
									   bool forceprefix, bool showimplicit,
									   int prettyFlags, int startIndent);
static char *pg_get_viewdef_worker(Oid viewoid,
								   int prettyFlags, int wrapColumn);
static char *pg_get_triggerdef_worker(Oid trigid, bool pretty);
static int	decompile_column_index_array(Datum column_index_array, Oid relId,
										 StringInfo buf);
static char *pg_get_ruledef_worker(Oid ruleoid, int prettyFlags);
static char *pg_get_indexdef_worker(Oid indexrelid, int colno,
									const Oid *excludeOps,
									bool attrsOnly, bool keysOnly,
									bool showTblSpc, bool inherits,
									int prettyFlags, bool missing_ok);
static char *pg_get_statisticsobj_worker(Oid statextid, bool columns_only,
										 bool missing_ok);
static char *pg_get_partkeydef_worker(Oid relid, int prettyFlags,
									  bool attrsOnly, bool missing_ok);
static char *pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
										 int prettyFlags, bool missing_ok);
static text *pg_get_expr_worker(text *expr, Oid relid, int prettyFlags);
static int	print_function_arguments(StringInfo buf, HeapTuple proctup,
									 bool print_table_args, bool print_defaults);
static void print_function_rettype(StringInfo buf, HeapTuple proctup);
static void print_function_trftypes(StringInfo buf, HeapTuple proctup);
static void print_function_sqlbody(StringInfo buf, HeapTuple proctup);
static void set_rtable_names(deparse_namespace *dpns, List *parent_namespaces,
							 Bitmapset *rels_used);
static void set_deparse_for_query(deparse_namespace *dpns, Query *query,
								  List *parent_namespaces);
static void set_simple_column_names(deparse_namespace *dpns);
static bool has_dangerous_join_using(deparse_namespace *dpns, Node *jtnode);
static void set_using_names(deparse_namespace *dpns, Node *jtnode,
							List *parentUsing);
static void set_relation_column_names(deparse_namespace *dpns,
									  RangeTblEntry *rte,
									  deparse_columns *colinfo);
static void set_join_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
								  deparse_columns *colinfo);
static bool colname_is_unique(const char *colname, deparse_namespace *dpns,
							  deparse_columns *colinfo);
static char *make_colname_unique(char *colname, deparse_namespace *dpns,
								 deparse_columns *colinfo);
static void expand_colnames_array_to(deparse_columns *colinfo, int n);
static void identify_join_columns(JoinExpr *j, RangeTblEntry *jrte,
								  deparse_columns *colinfo);
static char *get_rtable_name(int rtindex, deparse_context *context);
static void set_deparse_plan(deparse_namespace *dpns, Plan *plan);
static Plan *find_recursive_union(deparse_namespace *dpns,
								  WorkTableScan *wtscan);
static void push_child_plan(deparse_namespace *dpns, Plan *plan,
							deparse_namespace *save_dpns);
static void pop_child_plan(deparse_namespace *dpns,
						   deparse_namespace *save_dpns);
static void push_ancestor_plan(deparse_namespace *dpns, ListCell *ancestor_cell,
							   deparse_namespace *save_dpns);
static void pop_ancestor_plan(deparse_namespace *dpns,
							  deparse_namespace *save_dpns);
static void make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
						 int prettyFlags);
static void make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
						 int prettyFlags, int wrapColumn);
static void get_query_def(Query *query, StringInfo buf, List *parentnamespace,
						  TupleDesc resultDesc, bool colNamesVisible,
						  int prettyFlags, int wrapColumn, int startIndent);
static void get_values_def(List *values_lists, deparse_context *context);
static void get_with_clause(Query *query, deparse_context *context);
static void get_select_query_def(Query *query, deparse_context *context);
static void get_insert_query_def(Query *query, deparse_context *context);
static void get_update_query_def(Query *query, deparse_context *context);
static void get_update_query_targetlist_def(Query *query, List *targetList,
											deparse_context *context,
											RangeTblEntry *rte);
static void get_delete_query_def(Query *query, deparse_context *context);
static void get_merge_query_def(Query *query, deparse_context *context);
static void get_utility_query_def(Query *query, deparse_context *context);
static void get_basic_select_query(Query *query, deparse_context *context);
static void get_target_list(List *targetList, deparse_context *context);
static void get_setop_query(Node *setOp, Query *query,
							deparse_context *context);
static Node *get_rule_sortgroupclause(Index ref, List *tlist,
									  bool force_colno,
									  deparse_context *context);
static void get_rule_groupingset(GroupingSet *gset, List *targetlist,
								 bool omit_parens, deparse_context *context);
static void get_rule_orderby(List *orderList, List *targetList,
							 bool force_colno, deparse_context *context);
static void get_rule_windowclause(Query *query, deparse_context *context);
static void get_rule_windowspec(WindowClause *wc, List *targetList,
								deparse_context *context);
static char *get_variable(Var *var, int levelsup, bool istoplevel,
						  deparse_context *context);
static void get_special_variable(Node *node, deparse_context *context,
								 void *callback_arg);
static void resolve_special_varno(Node *node, deparse_context *context,
								  rsv_callback callback, void *callback_arg);
static Node *find_param_referent(Param *param, deparse_context *context,
								 deparse_namespace **dpns_p, ListCell **ancestor_cell_p);
static void get_parameter(Param *param, deparse_context *context);
static const char *get_simple_binary_op_name(OpExpr *expr);
static bool isSimpleNode(Node *node, Node *parentNode, int prettyFlags);
static void appendContextKeyword(deparse_context *context, const char *str,
								 int indentBefore, int indentAfter, int indentPlus);
static void removeStringInfoSpaces(StringInfo str);
static void get_rule_expr(Node *node, deparse_context *context,
						  bool showimplicit);
static void get_rule_expr_toplevel(Node *node, deparse_context *context,
								   bool showimplicit);
static void get_rule_list_toplevel(List *lst, deparse_context *context,
								   bool showimplicit);
static void get_rule_expr_funccall(Node *node, deparse_context *context,
								   bool showimplicit);
static bool looks_like_function(Node *node);
static void get_oper_expr(OpExpr *expr, deparse_context *context);
static void get_func_expr(FuncExpr *expr, deparse_context *context,
						  bool showimplicit);
static void get_agg_expr(Aggref *aggref, deparse_context *context,
						 Aggref *original_aggref);
static void get_agg_expr_helper(Aggref *aggref, deparse_context *context,
								Aggref *original_aggref, const char *funcname,
								const char *options, bool is_json_objectagg);
static void get_agg_combine_expr(Node *node, deparse_context *context,
								 void *callback_arg);
static void get_windowfunc_expr(WindowFunc *wfunc, deparse_context *context);
static void get_windowfunc_expr_helper(WindowFunc *wfunc, deparse_context *context,
									   const char *funcname, const char *options,
									   bool is_json_objectagg);
static bool get_func_sql_syntax(FuncExpr *expr, deparse_context *context);
static void get_coercion_expr(Node *arg, deparse_context *context,
							  Oid resulttype, int32 resulttypmod,
							  Node *parentNode);
static void get_const_expr(Const *constval, deparse_context *context,
						   int showtype);
static void get_const_collation(Const *constval, deparse_context *context);
static void get_json_format(JsonFormat *format, StringInfo buf);
static void get_json_constructor(JsonConstructorExpr *ctor,
								 deparse_context *context, bool showimplicit);
static void get_json_constructor_options(JsonConstructorExpr *ctor,
										 StringInfo buf);
static void get_json_agg_constructor(JsonConstructorExpr *ctor,
									 deparse_context *context,
									 const char *funcname,
									 bool is_json_objectagg);
static void simple_quote_literal(StringInfo buf, const char *val);
static void get_sublink_expr(SubLink *sublink, deparse_context *context);
static void get_tablefunc(TableFunc *tf, deparse_context *context,
						  bool showimplicit);
static void get_from_clause(Query *query, const char *prefix,
							deparse_context *context);
static void get_from_clause_item(Node *jtnode, Query *query,
								 deparse_context *context);
static void get_rte_alias(RangeTblEntry *rte, int varno, bool use_as,
						  deparse_context *context);
static void get_column_alias_list(deparse_columns *colinfo,
								  deparse_context *context);
static void get_from_clause_coldeflist(RangeTblFunction *rtfunc,
									   deparse_columns *colinfo,
									   deparse_context *context);
static void get_tablesample_def(TableSampleClause *tablesample,
								deparse_context *context);
static void get_opclass_name(Oid opclass, Oid actual_datatype,
							 StringInfo buf);
static Node *processIndirection(Node *node, deparse_context *context);
static void printSubscripts(SubscriptingRef *sbsref, deparse_context *context);
static char *get_relation_name(Oid relid);
static char *generate_relation_name(Oid relid, List *namespaces);
static char *generate_qualified_relation_name(Oid relid);
static char *generate_function_name(Oid funcid, int nargs,
									List *argnames, Oid *argtypes,
									bool has_variadic, bool *use_variadic_p,
									bool inGroupBy);
static char *generate_operator_name(Oid operid, Oid arg1, Oid arg2);
static void add_cast_to(StringInfo buf, Oid typid);
static char *generate_qualified_type_name(Oid typid);
static text *string_to_text(char *str);
static char *flatten_reloptions(Oid relid);
static void get_reloptions(StringInfo buf, Datum reloptions);

#define only_marker(rte)  ((rte)->inh ? "" : "ONLY ")


/* ----------
 * pg_get_ruledef		- 全做完并返回文本
 *					  其可用作一条重建
 *					  该规则的语句
 * ----------
 */
Datum
pg_get_ruledef(PG_FUNCTION_ARGS)
{
	Oid			ruleoid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_ruledef_worker(ruleoid, prettyFlags);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_ruledef_ext(PG_FUNCTION_ARGS)
{
	Oid			ruleoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_ruledef_worker(ruleoid, prettyFlags);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


static char *
pg_get_ruledef_worker(Oid ruleoid, int prettyFlags)
{
	Datum		args[1];
	char		nulls[1];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * 先做这步，使字符串在外层上下文而非 SPI 内分配
	 */
	initStringInfo(&buf);

	/*
	 * 连接 SPI 管理器
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 首次调用时准备 pg_rewrite 查找计划。经 SPI 管理
	 * 器读取 pg_rewrite，而不使用 syscache，以便对 pg_rewrite
	 * 的读权限进行检查。
	 */
	if (plan_getrulebyoid == NULL)
	{
		Oid			argtypes[1];
		SPIPlanPtr	plan;

		argtypes[0] = OIDOID;
		plan = SPI_prepare(query_getrulebyoid, 1, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getrulebyoid);
		SPI_keepplan(plan);
		plan_getrulebyoid = plan;
	}

	/*
	 * 取得此规则的 pg_rewrite 元组
	 */
	args[0] = ObjectIdGetDatum(ruleoid);
	nulls[0] = ' ';
	spirc = SPI_execute_plan(plan_getrulebyoid, args, nulls, true, 0);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for rule %u", ruleoid);
	if (SPI_processed != 1)
	{
		/*
		 * 此处没有元组数据，只需让输出缓冲区保持空。
		 *
		 */
	}
	else
	{
		/*
		 * 取得规则定义并将其放入执行器内存
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_ruledef(&buf, ruletup, rulettc, prettyFlags);
	}

	/*
	 * 断开 SPI 管理器连接
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	if (buf.len == 0)
		return NULL;

	return buf.data;
}


/* ----------
 * pg_get_viewdef		- 大体同上，但只返回
 *					  视图的 SELECT 部分
 * ----------
 */
Datum
pg_get_viewdef(PG_FUNCTION_ARGS)
{
	/* 按 OID*/
	Oid			viewoid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_viewdef_ext(PG_FUNCTION_ARGS)
{
	/* 按 OID*/
	Oid			viewoid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_viewdef_wrap(PG_FUNCTION_ARGS)
{
	/* 按 OID*/
	Oid			viewoid = PG_GETARG_OID(0);
	int			wrap = PG_GETARG_INT32(1);
	int			prettyFlags;
	char	   *res;

	/* 调用此函数即意味着想要美化打印*/
	prettyFlags = GET_PRETTY_FLAGS(true);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, wrap);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_viewdef_name(PG_FUNCTION_ARGS)
{
	/* 按限定名 */
	text	   *viewname = PG_GETARG_TEXT_PP(0);
	int			prettyFlags;
	RangeVar   *viewrel;
	Oid			viewoid;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	/* 查找视图名。不能加锁 —— 我们可能没有权限。 */
	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, NoLock, false);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}


Datum
pg_get_viewdef_name_ext(PG_FUNCTION_ARGS)
{
	/* 按限定名 */
	text	   *viewname = PG_GETARG_TEXT_PP(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	RangeVar   *viewrel;
	Oid			viewoid;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	/* 查找视图名。不能加锁 —— 我们可能没有权限。 */
	viewrel = makeRangeVarFromNameList(textToQualifiedNameList(viewname));
	viewoid = RangeVarGetRelid(viewrel, NoLock, false);

	res = pg_get_viewdef_worker(viewoid, prettyFlags, WRAP_COLUMN_DEFAULT);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * pg_get_viewdef 按 OID 与按名两种变体的公共代码
 */
static char *
pg_get_viewdef_worker(Oid viewoid, int prettyFlags, int wrapColumn)
{
	Datum		args[2];
	char		nulls[2];
	int			spirc;
	HeapTuple	ruletup;
	TupleDesc	rulettc;
	StringInfoData buf;

	/*
	 * 先做这步，使字符串在外层上下文而非 SPI 内分配
	 */
	initStringInfo(&buf);

	/*
	 * 连接 SPI 管理器
	 */
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * 首次调用时准备 pg_rewrite 查找计划。经 SPI 管理
	 * 器读取 pg_rewrite，而不使用 syscache，以便对 pg_rewrite
	 * 的读权限进行检查。
	 */
	if (plan_getviewrule == NULL)
	{
		Oid			argtypes[2];
		SPIPlanPtr	plan;

		argtypes[0] = OIDOID;
		argtypes[1] = NAMEOID;
		plan = SPI_prepare(query_getviewrule, 2, argtypes);
		if (plan == NULL)
			elog(ERROR, "SPI_prepare failed for \"%s\"", query_getviewrule);
		SPI_keepplan(plan);
		plan_getviewrule = plan;
	}

	/*
	 * 取得视图 SELECT 规则的 pg_rewrite 元组
	 */
	args[0] = ObjectIdGetDatum(viewoid);
	args[1] = DirectFunctionCall1(namein, CStringGetDatum(ViewSelectRuleName));
	nulls[0] = ' ';
	nulls[1] = ' ';
	spirc = SPI_execute_plan(plan_getviewrule, args, nulls, true, 0);
	if (spirc != SPI_OK_SELECT)
		elog(ERROR, "failed to get pg_rewrite tuple for view %u", viewoid);
	if (SPI_processed != 1)
	{
		/*
		 * 此处没有元组数据，只需让输出缓冲区保持空。
		 *
		 */
	}
	else
	{
		/*
		 * 取得规则定义并将其放入执行器内存
		 */
		ruletup = SPI_tuptable->vals[0];
		rulettc = SPI_tuptable->tupdesc;
		make_viewdef(&buf, ruletup, rulettc, prettyFlags, wrapColumn);
	}

	/*
	 * 断开 SPI 管理器连接
	 */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	if (buf.len == 0)
		return NULL;

	return buf.data;
}

/* ----------
 * pg_get_triggerdef		- 获取触发器的定义
 * ----------
 */
Datum
pg_get_triggerdef(PG_FUNCTION_ARGS)
{
	Oid			trigid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_triggerdef_worker(trigid, false);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_triggerdef_ext(PG_FUNCTION_ARGS)
{
	Oid			trigid = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	char	   *res;

	res = pg_get_triggerdef_worker(trigid, pretty);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

static char *
pg_get_triggerdef_worker(Oid trigid, bool pretty)
{
	HeapTuple	ht_trig;
	Form_pg_trigger trigrec;
	StringInfoData buf;
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	int			findx = 0;
	char	   *tgname;
	char	   *tgoldtable;
	char	   *tgnewtable;
	Datum		value;
	bool		isnull;

	/*
	 * 按触发器的 OID 取得 pg_trigger 元组
	 */
	tgrel = table_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								NULL, 1, skey);

	ht_trig = systable_getnext(tgscan);

	if (!HeapTupleIsValid(ht_trig))
	{
		systable_endscan(tgscan);
		table_close(tgrel, AccessShareLock);
		return NULL;
	}

	trigrec = (Form_pg_trigger) GETSTRUCT(ht_trig);

	/*
	 * 开始触发器定义。注意触发器的名字永远不应带模式
	 * 限定，但触发器所在关系的名字可以带。
	 */
	initStringInfo(&buf);

	tgname = NameStr(trigrec->tgname);
	appendStringInfo(&buf, "CREATE %sTRIGGER %s ",
					 OidIsValid(trigrec->tgconstraint) ? "CONSTRAINT " : "",
					 quote_identifier(tgname));

	if (TRIGGER_FOR_BEFORE(trigrec->tgtype))
		appendStringInfoString(&buf, "BEFORE");
	else if (TRIGGER_FOR_AFTER(trigrec->tgtype))
		appendStringInfoString(&buf, "AFTER");
	else if (TRIGGER_FOR_INSTEAD(trigrec->tgtype))
		appendStringInfoString(&buf, "INSTEAD OF");
	else
		elog(ERROR, "unexpected tgtype value: %d", trigrec->tgtype);

	if (TRIGGER_FOR_INSERT(trigrec->tgtype))
	{
		appendStringInfoString(&buf, " INSERT");
		findx++;
	}
	if (TRIGGER_FOR_DELETE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR DELETE");
		else
			appendStringInfoString(&buf, " DELETE");
		findx++;
	}
	if (TRIGGER_FOR_UPDATE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR UPDATE");
		else
			appendStringInfoString(&buf, " UPDATE");
		findx++;
		/* tgattr 是首个变宽字段，因此可以直接访问 */
		if (trigrec->tgattr.dim1 > 0)
		{
			int			i;

			appendStringInfoString(&buf, " OF ");
			for (i = 0; i < trigrec->tgattr.dim1; i++)
			{
				char	   *attname;

				if (i > 0)
					appendStringInfoString(&buf, ", ");
				attname = get_attname(trigrec->tgrelid,
									  trigrec->tgattr.values[i], false);
				appendStringInfoString(&buf, quote_identifier(attname));
			}
		}
	}
	if (TRIGGER_FOR_TRUNCATE(trigrec->tgtype))
	{
		if (findx > 0)
			appendStringInfoString(&buf, " OR TRUNCATE");
		else
			appendStringInfoString(&buf, " TRUNCATE");
		findx++;
	}

	/*
	 * 非美化模式下为安全起见总是模式限定目标表名。
	 * 美化模式下，仅在不可见时才加模式限定。
	 */
	appendStringInfo(&buf, " ON %s ",
					 pretty ?
					 generate_relation_name(trigrec->tgrelid, NIL) :
					 generate_qualified_relation_name(trigrec->tgrelid));

	if (OidIsValid(trigrec->tgconstraint))
	{
		if (OidIsValid(trigrec->tgconstrrelid))
			appendStringInfo(&buf, "FROM %s ",
							 generate_relation_name(trigrec->tgconstrrelid, NIL));
		if (!trigrec->tgdeferrable)
			appendStringInfoString(&buf, "NOT ");
		appendStringInfoString(&buf, "DEFERRABLE INITIALLY ");
		if (trigrec->tginitdeferred)
			appendStringInfoString(&buf, "DEFERRED ");
		else
			appendStringInfoString(&buf, "IMMEDIATE ");
	}

	value = fastgetattr(ht_trig, Anum_pg_trigger_tgoldtable,
						tgrel->rd_att, &isnull);
	if (!isnull)
		tgoldtable = NameStr(*DatumGetName(value));
	else
		tgoldtable = NULL;
	value = fastgetattr(ht_trig, Anum_pg_trigger_tgnewtable,
						tgrel->rd_att, &isnull);
	if (!isnull)
		tgnewtable = NameStr(*DatumGetName(value));
	else
		tgnewtable = NULL;
	if (tgoldtable != NULL || tgnewtable != NULL)
	{
		appendStringInfoString(&buf, "REFERENCING ");
		if (tgoldtable != NULL)
			appendStringInfo(&buf, "OLD TABLE AS %s ",
							 quote_identifier(tgoldtable));
		if (tgnewtable != NULL)
			appendStringInfo(&buf, "NEW TABLE AS %s ",
							 quote_identifier(tgnewtable));
	}

	if (TRIGGER_FOR_ROW(trigrec->tgtype))
		appendStringInfoString(&buf, "FOR EACH ROW ");
	else
		appendStringInfoString(&buf, "FOR EACH STATEMENT ");

	/*若触发器有 WHEN 限定条件，则将其加上*/
	value = fastgetattr(ht_trig, Anum_pg_trigger_tgqual,
						tgrel->rd_att, &isnull);
	if (!isnull)
	{
		Node	   *qual;
		char		relkind;
		deparse_context context;
		deparse_namespace dpns;
		RangeTblEntry *oldrte;
		RangeTblEntry *newrte;

		appendStringInfoString(&buf, "WHEN (");

		qual = stringToNode(TextDatumGetCString(value));

		relkind = get_rel_relkind(trigrec->tgrelid);

		/* 为该关系构建最小的 OLD 与 NEW RTE*/
		oldrte = makeNode(RangeTblEntry);
		oldrte->rtekind = RTE_RELATION;
		oldrte->relid = trigrec->tgrelid;
		oldrte->relkind = relkind;
		oldrte->rellockmode = AccessShareLock;
		oldrte->alias = makeAlias("old", NIL);
		oldrte->eref = oldrte->alias;
		oldrte->lateral = false;
		oldrte->inh = false;
		oldrte->inFromCl = true;

		newrte = makeNode(RangeTblEntry);
		newrte->rtekind = RTE_RELATION;
		newrte->relid = trigrec->tgrelid;
		newrte->relkind = relkind;
		newrte->rellockmode = AccessShareLock;
		newrte->alias = makeAlias("new", NIL);
		newrte->eref = newrte->alias;
		newrte->lateral = false;
		newrte->inh = false;
		newrte->inFromCl = true;

		/* 构建两元素范围表 */
		memset(&dpns, 0, sizeof(dpns));
		dpns.rtable = list_make2(oldrte, newrte);
		dpns.subplans = NIL;
		dpns.ctes = NIL;
		dpns.appendrels = NULL;
		set_rtable_names(&dpns, NIL, NULL);
		set_simple_column_names(&dpns);

		/* 建立带一层命名空间栈的上下文 */
		context.buf = &buf;
		context.namespaces = list_make1(&dpns);
		context.resultDesc = NULL;
		context.targetList = NIL;
		context.windowClause = NIL;
		context.varprefix = true;
		context.prettyFlags = GET_PRETTY_FLAGS(pretty);
		context.wrapColumn = WRAP_COLUMN_DEFAULT;
		context.indentLevel = PRETTYINDENT_STD;
		context.colNamesVisible = true;
		context.inGroupBy = false;
		context.varInOrderBy = false;
		context.appendparents = NULL;

		get_rule_expr(qual, &context, false);

		appendStringInfoString(&buf, ") ");
	}

	appendStringInfo(&buf, "EXECUTE FUNCTION %s(",
					 generate_function_name(trigrec->tgfoid, 0,
											NIL, NULL,
											false, NULL, false));

	if (trigrec->tgnargs > 0)
	{
		char	   *p;
		int			i;

		value = fastgetattr(ht_trig, Anum_pg_trigger_tgargs,
							tgrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "tgargs is null for trigger %u", trigid);
		p = (char *) VARDATA_ANY(DatumGetByteaPP(value));
		for (i = 0; i < trigrec->tgnargs; i++)
		{
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			simple_quote_literal(&buf, p);
			/* p 推进到 tgargs 内的下一个字符串 */
			while (*p)
				p++;
			p++;
		}
	}

	/* 我们刻意不在结尾加分号 */
	appendStringInfoChar(&buf, ')');

	/* 清理 */
	systable_endscan(tgscan);

	table_close(tgrel, AccessShareLock);

	return buf.data;
}

/* ----------
 * pg_get_indexdef			- 获取索引的定义
 *
 * 在扩展版本中，还有 colno 参数以及 pretty 布尔量。
 *		if colno == 0，我们要完整的索引定义。
 *		if colno > 0，只要第 N 个索引键的变量或表达式。
 *
 * 注意本函数的 SQL 版本会省略索引表空间的信息；
 * 这是有意为之，因为 pg_dump 希望如此。然而
 * pg_get_indexdef_string() 会包含索引表空间。
 * ----------
 */
Datum
pg_get_indexdef(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_indexdef_worker(indexrelid, 0, NULL,
								 false, false,
								 false, false,
								 prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_indexdef_ext(PG_FUNCTION_ARGS)
{
	Oid			indexrelid = PG_GETARG_OID(0);
	int32		colno = PG_GETARG_INT32(1);
	bool		pretty = PG_GETARG_BOOL(2);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_indexdef_worker(indexrelid, colno, NULL,
								 colno != 0, false,
								 false, false,
								 prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * 供 ALTER TABLE 用的内部版本。
 * 结果中包含表空间子句。
 * 返回 palloc 的 C 串；不做美化打印。
 */
char *
pg_get_indexdef_string(Oid indexrelid)
{
	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  false, false,
								  true, true,
								  0, false);
}

/* 只报告键列定义的内部版本 */
char *
pg_get_indexdef_columns(Oid indexrelid, bool pretty)
{
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  true, true,
								  false, false,
								  prettyFlags, false);
}

/* 内部版本，可通过 flags 扩展以控制其行为 */
char *
pg_get_indexdef_columns_extended(Oid indexrelid, bits16 flags)
{
	bool		pretty = ((flags & RULE_INDEXDEF_PRETTY) != 0);
	bool		keys_only = ((flags & RULE_INDEXDEF_KEYS_ONLY) != 0);
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_indexdef_worker(indexrelid, 0, NULL,
								  true, keys_only,
								  false, false,
								  prettyFlags, false);
}

/*
 * 反解析索引定义的内部主力函数。
 *
 * 现在它也用于排除约束：如果 excludeOps 非 NULL，
 * 则它指向一个由排除操作符 OID 组成的数组。
 */
static char *
pg_get_indexdef_worker(Oid indexrelid, int colno,
					   const Oid *excludeOps,
					   bool attrsOnly, bool keysOnly,
					   bool showTblSpc, bool inherits,
					   int prettyFlags, bool missing_ok)
{
	/* 以后可能想要单独的 isConstraint 参数 */
	bool		isConstraint = (excludeOps != NULL);
	HeapTuple	ht_idx;
	HeapTuple	ht_idxrel;
	HeapTuple	ht_am;
	Form_pg_index idxrec;
	Form_pg_class idxrelrec;
	Form_pg_am	amrec;
	IndexAmRoutine *amroutine;
	List	   *indexprs;
	ListCell   *indexpr_item;
	List	   *context;
	Oid			indrelid;
	int			keyno;
	Datum		indcollDatum;
	Datum		indclassDatum;
	Datum		indoptionDatum;
	oidvector  *indcollation;
	oidvector  *indclass;
	int2vector *indoption;
	StringInfoData buf;
	char	   *str;
	char	   *sep;

	/*
	 * 按索引的 OID 取得 pg_index 元组
	 */
	ht_idx = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexrelid));
	if (!HeapTupleIsValid(ht_idx))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for index %u", indexrelid);
	}
	idxrec = (Form_pg_index) GETSTRUCT(ht_idx);

	indrelid = idxrec->indrelid;
	Assert(indexrelid == idxrec->indexrelid);

	/* 只能费劲获取 indcollation、indclass 和 indoption */
	indcollDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
										  Anum_pg_index_indcollation);
	indcollation = (oidvector *) DatumGetPointer(indcollDatum);

	indclassDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
										   Anum_pg_index_indclass);
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	indoptionDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											Anum_pg_index_indoption);
	indoption = (int2vector *) DatumGetPointer(indoptionDatum);

	/*
	 * 取得索引关系的 pg_class 元组
	 */
	ht_idxrel = SearchSysCache1(RELOID, ObjectIdGetDatum(indexrelid));
	if (!HeapTupleIsValid(ht_idxrel))
		elog(ERROR, "cache lookup failed for relation %u", indexrelid);
	idxrelrec = (Form_pg_class) GETSTRUCT(ht_idxrel);

	/*
	 * 取得索引访问方法的 pg_am 元组
	 */
	ht_am = SearchSysCache1(AMOID, ObjectIdGetDatum(idxrelrec->relam));
	if (!HeapTupleIsValid(ht_am))
		elog(ERROR, "cache lookup failed for access method %u",
			 idxrelrec->relam);
	amrec = (Form_pg_am) GETSTRUCT(ht_am);

	/* 取得索引 AM 的 API 结构 */
	amroutine = GetIndexAmRoutine(amrec->amhandler);

	/*
	 * 取得索引表达式，若有。（注意：我们不使用 relcache
	 * 中的表达式与谓词版本，因为我们想显示未常量折叠的
	 * 表达式。）
	 */
	if (!heap_attisnull(ht_idx, Anum_pg_index_indexprs, NULL))
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											Anum_pg_index_indexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		indexprs = (List *) stringToNode(exprsString);
		pfree(exprsString);
	}
	else
		indexprs = NIL;

	indexpr_item = list_head(indexprs);

	context = deparse_context_for(get_relation_name(indrelid), indrelid);

	/*
	 * 开始索引定义。注意索引名字永远不应带模式限定，
	 * 但被索引关系的名字可以带。
	 */
	initStringInfo(&buf);

	if (!attrsOnly)
	{
		if (!isConstraint)
			appendStringInfo(&buf, "CREATE %sINDEX %s ON %s%s USING %s (",
							 idxrec->indisunique ? "UNIQUE " : "",
							 quote_identifier(NameStr(idxrelrec->relname)),
							 idxrelrec->relkind == RELKIND_PARTITIONED_INDEX
							 && !inherits ? "ONLY " : "",
							 (prettyFlags & PRETTYFLAG_SCHEMA) ?
							 generate_relation_name(indrelid, NIL) :
							 generate_qualified_relation_name(indrelid),
							 quote_identifier(NameStr(amrec->amname)));
		else					/* 目前，必须是 EXCLUDE 约束 */
			appendStringInfo(&buf, "EXCLUDE USING %s (",
							 quote_identifier(NameStr(amrec->amname)));
	}

	/*
	 * 报告被索引的属性
	 */
	sep = "";
	for (keyno = 0; keyno < idxrec->indnatts; keyno++)
	{
		AttrNumber	attnum = idxrec->indkey.values[keyno];
		Oid			keycoltype;
		Oid			keycolcollation;

		/*
		 * 若被要求，忽略非键属性。
		 */
		if (keysOnly && keyno >= idxrec->indnkeyatts)
			break;

		/* 否则，打印 INCLUDE 以隔开键与非键属性。 */
		if (!colno && keyno == idxrec->indnkeyatts)
		{
			appendStringInfoString(&buf, ") INCLUDE (");
			sep = "";
		}

		if (!colno)
			appendStringInfoString(&buf, sep);
		sep = ", ";

		if (attnum != 0)
		{
			/* 简单索引列 */
			char	   *attname;
			int32		keycoltypmod;

			attname = get_attname(indrelid, attnum, false);
			if (!colno || colno == keyno + 1)
				appendStringInfoString(&buf, quote_identifier(attname));
			get_atttypetypmodcoll(indrelid, attnum,
								  &keycoltype, &keycoltypmod,
								  &keycolcollation);
		}
		else
		{
			/* 表达式索引 */
			Node	   *indexkey;

			if (indexpr_item == NULL)
				elog(ERROR, "too few entries in indexprs list");
			indexkey = (Node *) lfirst(indexpr_item);
			indexpr_item = lnext(indexprs, indexpr_item);
			/*反解析*/
			str = deparse_expression_pretty(indexkey, context, false, false,
											prettyFlags, 0);
			if (!colno || colno == keyno + 1)
			{
				/* 如果不是纯函数调用则需要括号 */
				if (looks_like_function(indexkey))
					appendStringInfoString(&buf, str);
				else
					appendStringInfo(&buf, "(%s)", str);
			}
			keycoltype = exprType(indexkey);
			keycolcollation = exprCollation(indexkey);
		}

		/* 为（选定的）键列打印附加的修饰 */
		if (!attrsOnly && keyno < idxrec->indnkeyatts &&
			(!colno || colno == keyno + 1))
		{
			int16		opt = indoption->values[keyno];
			Oid			indcoll = indcollation->values[keyno];
			Datum		attoptions = get_attoptions(indexrelid, keyno + 1);
			bool		has_options = attoptions != (Datum) 0;

			/* 非列默认时添加排序规则 */
			if (OidIsValid(indcoll) && indcoll != keycolcollation)
				appendStringInfo(&buf, " COLLATE %s",
								 generate_collation_name((indcoll)));

			/* 若非默认则添加操作符类名 */
			get_opclass_name(indclass->values[keyno],
							 has_options ? InvalidOid : keycoltype, &buf);

			if (has_options)
			{
				appendStringInfoString(&buf, " (");
				get_reloptions(&buf, attoptions);
				appendStringInfoChar(&buf, ')');
			}

			/* 如相关则添加选项*/
			if (amroutine->amcanorder)
			{
				/* 如果支持排序，报告 DESC 与 NULLS 选项 */
				if (opt & INDOPTION_DESC)
				{
					appendStringInfoString(&buf, " DESC");
					/* 这种情况下 NULLS FIRST 是默认值*/
					if (!(opt & INDOPTION_NULLS_FIRST))
						appendStringInfoString(&buf, " NULLS LAST");
				}
				else
				{
					if (opt & INDOPTION_NULLS_FIRST)
						appendStringInfoString(&buf, " NULLS FIRST");
				}
			}

			/* 如相关则添加排除操作符 */
			if (excludeOps != NULL)
				appendStringInfo(&buf, " WITH %s",
								 generate_operator_name(excludeOps[keyno],
														keycoltype,
														keycoltype));
		}
	}

	if (!attrsOnly)
	{
		appendStringInfoChar(&buf, ')');

		if (idxrec->indnullsnotdistinct)
			appendStringInfoString(&buf, " NULLS NOT DISTINCT");

		/*
		 * 如有选项，追加 "WITH (options)"
		 */
		str = flatten_reloptions(indexrelid);
		if (str)
		{
			appendStringInfo(&buf, " WITH (%s)", str);
			pfree(str);
		}

		/*
		 * 打印表空间，但仅按要求
		 */
		if (showTblSpc)
		{
			Oid			tblspc;

			tblspc = get_rel_tablespace(indexrelid);
			if (OidIsValid(tblspc))
			{
				if (isConstraint)
					appendStringInfoString(&buf, " USING INDEX");
				appendStringInfo(&buf, " TABLESPACE %s",
								 quote_identifier(get_tablespace_name(tblspc)));
			}
		}

		/*
		 * 如果是部分索引，则反解析并追加谓词
		 */
		if (!heap_attisnull(ht_idx, Anum_pg_index_indpred, NULL))
		{
			Node	   *node;
			Datum		predDatum;
			char	   *predString;

			/* 把文本串转换为节点树 */
			predDatum = SysCacheGetAttrNotNull(INDEXRELID, ht_idx,
											   Anum_pg_index_indpred);
			predString = TextDatumGetCString(predDatum);
			node = (Node *) stringToNode(predString);
			pfree(predString);

			/*反解析*/
			str = deparse_expression_pretty(node, context, false, false,
											prettyFlags, 0);
			if (isConstraint)
				appendStringInfo(&buf, " WHERE (%s)", str);
			else
				appendStringInfo(&buf, " WHERE %s", str);
		}
	}

	/* 清理 */
	ReleaseSysCache(ht_idx);
	ReleaseSysCache(ht_idxrel);
	ReleaseSysCache(ht_am);

	return buf.data;
}

/* ----------
 * pg_get_querydef
 *
 * 反解析单个查询解析树的入口点。
 * 美化标志由 GET_PRETTY_FLAGS(pretty) 决定。
 *
 * 返回 palloc 的 C 字符串。
 * ----------
 */
char *
pg_get_querydef(Query *query, bool pretty)
{
	StringInfoData buf;
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	initStringInfo(&buf);

	get_query_def(query, &buf, NIL, NULL, true,
				  prettyFlags, WRAP_COLUMN_DEFAULT, 0);

	return buf.data;
}

/*
 * pg_get_statisticsobjdef
 *			取得扩展统计对象的定义
 */
Datum
pg_get_statisticsobjdef(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_statisticsobj_worker(statextid, false, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * 供 ALTER TABLE 用的内部版本。
 * 结果中包含表空间子句。
 * 返回 palloc 的 C 串；不做美化打印。
 */
char *
pg_get_statisticsobjdef_string(Oid statextid)
{
	return pg_get_statisticsobj_worker(statextid, false, false);
}

/*
 * pg_get_statisticsobjdef_columns
 *			取得扩展统计对象的列与表达式
 */
Datum
pg_get_statisticsobjdef_columns(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_statisticsobj_worker(statextid, true, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * 反解析扩展统计对象的内部主力函数。
 */
static char *
pg_get_statisticsobj_worker(Oid statextid, bool columns_only, bool missing_ok)
{
	Form_pg_statistic_ext statextrec;
	HeapTuple	statexttup;
	StringInfoData buf;
	int			colno;
	char	   *nsp;
	ArrayType  *arr;
	char	   *enabled;
	Datum		datum;
	bool		ndistinct_enabled;
	bool		dependencies_enabled;
	bool		mcv_enabled;
	int			i;
	List	   *context;
	ListCell   *lc;
	List	   *exprs = NIL;
	bool		has_exprs;
	int			ncolumns;

	statexttup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statextid));

	if (!HeapTupleIsValid(statexttup))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for statistics object %u", statextid);
	}

	/* 是否有统计表达式？ */
	has_exprs = !heap_attisnull(statexttup, Anum_pg_statistic_ext_stxexprs, NULL);

	statextrec = (Form_pg_statistic_ext) GETSTRUCT(statexttup);

	/*
	 * 取得统计表达式，若有。（注意：不用 relcache 的
	 * 表达式版本，因为我们想显示未常量折叠的
	 * 表达式。）
	 */
	if (has_exprs)
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
											Anum_pg_statistic_ext_stxexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		exprs = (List *) stringToNode(exprsString);
		pfree(exprsString);
	}
	else
		exprs = NIL;

	/* 统计列数（属性与表达式） */
	ncolumns = statextrec->stxkeys.dim1 + list_length(exprs);

	initStringInfo(&buf);

	if (!columns_only)
	{
		nsp = get_namespace_name_or_temp(statextrec->stxnamespace);
		appendStringInfo(&buf, "CREATE STATISTICS %s",
						 quote_qualified_identifier(nsp,
													NameStr(statextrec->stxname)));

		/*
		 * 解码 stxkind 列，以了解要打印哪些统计类型。
		 *
		 */
		datum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
									   Anum_pg_statistic_ext_stxkind);
		arr = DatumGetArrayTypeP(datum);
		if (ARR_NDIM(arr) != 1 ||
			ARR_HASNULL(arr) ||
			ARR_ELEMTYPE(arr) != CHAROID)
			elog(ERROR, "stxkind is not a 1-D char array");
		enabled = (char *) ARR_DATA_PTR(arr);

		ndistinct_enabled = false;
		dependencies_enabled = false;
		mcv_enabled = false;

		for (i = 0; i < ARR_DIMS(arr)[0]; i++)
		{
			if (enabled[i] == STATS_EXT_NDISTINCT)
				ndistinct_enabled = true;
			else if (enabled[i] == STATS_EXT_DEPENDENCIES)
				dependencies_enabled = true;
			else if (enabled[i] == STATS_EXT_MCV)
				mcv_enabled = true;

			/* 忽略 STATS_EXT_EXPRESSIONS（它是自动构建的） */
		}

		/*
		 * 如果任何一个选项被禁用，就需要追加 types
		 * 子句显示启用了哪些项。全启用时，我们故意省略
		 * 该子句：若统计在原版本上启用了全部选项，则
		 * pg_dump/pg_restore 在较新的 postgres 版本上就会创建
		 * 全部统计类型。
		 *
		 * 但统计对象若仅定义在单个列上，就必须是表达式
		 * 统计；那种情况下我们不需要指定 kinds。
		 *
		 */
		if ((!ndistinct_enabled || !dependencies_enabled || !mcv_enabled) &&
			(ncolumns > 1))
		{
			bool		gotone = false;

			appendStringInfoString(&buf, " (");

			if (ndistinct_enabled)
			{
				appendStringInfoString(&buf, "ndistinct");
				gotone = true;
			}

			if (dependencies_enabled)
			{
				appendStringInfo(&buf, "%sdependencies", gotone ? ", " : "");
				gotone = true;
			}

			if (mcv_enabled)
				appendStringInfo(&buf, "%smcv", gotone ? ", " : "");

			appendStringInfoChar(&buf, ')');
		}

		appendStringInfoString(&buf, " ON ");
	}

	/* 解码简单列引用 */
	for (colno = 0; colno < statextrec->stxkeys.dim1; colno++)
	{
		AttrNumber	attnum = statextrec->stxkeys.values[colno];
		char	   *attname;

		if (colno > 0)
			appendStringInfoString(&buf, ", ");

		attname = get_attname(statextrec->stxrelid, attnum, false);

		appendStringInfoString(&buf, quote_identifier(attname));
	}

	context = deparse_context_for(get_relation_name(statextrec->stxrelid),
								  statextrec->stxrelid);

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		char	   *str;
		int			prettyFlags = PRETTYFLAG_PAREN;

		str = deparse_expression_pretty(expr, context, false, false,
										prettyFlags, 0);

		if (colno > 0)
			appendStringInfoString(&buf, ", ");

		/* 如果不是纯函数调用则需要括号 */
		if (looks_like_function(expr))
			appendStringInfoString(&buf, str);
		else
			appendStringInfo(&buf, "(%s)", str);

		colno++;
	}

	if (!columns_only)
		appendStringInfo(&buf, " FROM %s",
						 generate_relation_name(statextrec->stxrelid, NIL));

	ReleaseSysCache(statexttup);

	return buf.data;
}

/*
 * 为统计对象生成表达式的文本数组。
 */
Datum
pg_get_statisticsobjdef_expressions(PG_FUNCTION_ARGS)
{
	Oid			statextid = PG_GETARG_OID(0);
	Form_pg_statistic_ext statextrec;
	HeapTuple	statexttup;
	Datum		datum;
	List	   *context;
	ListCell   *lc;
	List	   *exprs = NIL;
	bool		has_exprs;
	char	   *tmp;
	ArrayBuildState *astate = NULL;

	statexttup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(statextid));

	if (!HeapTupleIsValid(statexttup))
		PG_RETURN_NULL();

	/* 该统计对象有表达式吗？ */
	has_exprs = !heap_attisnull(statexttup, Anum_pg_statistic_ext_stxexprs, NULL);

	/* 无表达式？结束 */
	if (!has_exprs)
	{
		ReleaseSysCache(statexttup);
		PG_RETURN_NULL();
	}

	statextrec = (Form_pg_statistic_ext) GETSTRUCT(statexttup);

	/*
	 * 取得统计表达式，并将其反解析成文本值。
	 */
	datum = SysCacheGetAttrNotNull(STATEXTOID, statexttup,
								   Anum_pg_statistic_ext_stxexprs);
	tmp = TextDatumGetCString(datum);
	exprs = (List *) stringToNode(tmp);
	pfree(tmp);

	context = deparse_context_for(get_relation_name(statextrec->stxrelid),
								  statextrec->stxrelid);

	foreach(lc, exprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		char	   *str;
		int			prettyFlags = PRETTYFLAG_INDENT;

		str = deparse_expression_pretty(expr, context, false, false,
										prettyFlags, 0);

		astate = accumArrayResult(astate,
								  PointerGetDatum(cstring_to_text(str)),
								  false,
								  TEXTOID,
								  CurrentMemoryContext);
	}

	ReleaseSysCache(statexttup);

	PG_RETURN_DATUM(makeArrayResult(astate, CurrentMemoryContext));
}

/*
 * pg_get_partkeydef
 *
 * 返回分区键说明，即如下形式：
 *
 * { RANGE | LIST | HASH } (column opt_collation opt_opclass [, ...])
 */
Datum
pg_get_partkeydef(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	char	   *res;

	res = pg_get_partkeydef_worker(relid, PRETTYFLAG_INDENT, false, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/* 只报告列定义的内部版本 */
char *
pg_get_partkeydef_columns(Oid relid, bool pretty)
{
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	return pg_get_partkeydef_worker(relid, prettyFlags, true, false);
}

/*
 * 反解析分区键定义的内部主力函数。
 */
static char *
pg_get_partkeydef_worker(Oid relid, int prettyFlags,
						 bool attrsOnly, bool missing_ok)
{
	Form_pg_partitioned_table form;
	HeapTuple	tuple;
	oidvector  *partclass;
	oidvector  *partcollation;
	List	   *partexprs;
	ListCell   *partexpr_item;
	List	   *context;
	Datum		datum;
	StringInfoData buf;
	int			keyno;
	char	   *str;
	char	   *sep;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
	{
		if (missing_ok)
			return NULL;
		elog(ERROR, "cache lookup failed for partition key of %u", relid);
	}

	form = (Form_pg_partitioned_table) GETSTRUCT(tuple);

	Assert(form->partrelid == relid);

	/* 只能费劲获取 partclass 与 partcollation */
	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partclass);
	partclass = (oidvector *) DatumGetPointer(datum);

	datum = SysCacheGetAttrNotNull(PARTRELID, tuple,
								   Anum_pg_partitioned_table_partcollation);
	partcollation = (oidvector *) DatumGetPointer(datum);


	/*
	 * 取得表达式，若有。（注意：我们不使用 relcache
	 * 版本的表达式，因为我们想显示未常量折叠的
	 * 表达式。）
	 */
	if (!heap_attisnull(tuple, Anum_pg_partitioned_table_partexprs, NULL))
	{
		Datum		exprsDatum;
		char	   *exprsString;

		exprsDatum = SysCacheGetAttrNotNull(PARTRELID, tuple,
											Anum_pg_partitioned_table_partexprs);
		exprsString = TextDatumGetCString(exprsDatum);
		partexprs = (List *) stringToNode(exprsString);

		if (!IsA(partexprs, List))
			elog(ERROR, "unexpected node type found in partexprs: %d",
				 (int) nodeTag(partexprs));

		pfree(exprsString);
	}
	else
		partexprs = NIL;

	partexpr_item = list_head(partexprs);
	context = deparse_context_for(get_relation_name(relid), relid);

	initStringInfo(&buf);

	switch (form->partstrat)
	{
		case PARTITION_STRATEGY_HASH:
			if (!attrsOnly)
				appendStringInfoString(&buf, "HASH");
			break;
		case PARTITION_STRATEGY_LIST:
			if (!attrsOnly)
				appendStringInfoString(&buf, "LIST");
			break;
		case PARTITION_STRATEGY_RANGE:
			if (!attrsOnly)
				appendStringInfoString(&buf, "RANGE");
			break;
		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) form->partstrat);
	}

	if (!attrsOnly)
		appendStringInfoString(&buf, " (");
	sep = "";
	for (keyno = 0; keyno < form->partnatts; keyno++)
	{
		AttrNumber	attnum = form->partattrs.values[keyno];
		Oid			keycoltype;
		Oid			keycolcollation;
		Oid			partcoll;

		appendStringInfoString(&buf, sep);
		sep = ", ";
		if (attnum != 0)
		{
			/* 简单属性引用 */
			char	   *attname;
			int32		keycoltypmod;

			attname = get_attname(relid, attnum, false);
			appendStringInfoString(&buf, quote_identifier(attname));
			get_atttypetypmodcoll(relid, attnum,
								  &keycoltype, &keycoltypmod,
								  &keycolcollation);
		}
		else
		{
			/* 表达式 */
			Node	   *partkey;

			if (partexpr_item == NULL)
				elog(ERROR, "too few entries in partexprs list");
			partkey = (Node *) lfirst(partexpr_item);
			partexpr_item = lnext(partexprs, partexpr_item);

			/*反解析*/
			str = deparse_expression_pretty(partkey, context, false, false,
											prettyFlags, 0);
			/* 如果不是纯函数调用则需要括号 */
			if (looks_like_function(partkey))
				appendStringInfoString(&buf, str);
			else
				appendStringInfo(&buf, "(%s)", str);

			keycoltype = exprType(partkey);
			keycolcollation = exprCollation(partkey);
		}

		/* 非列默认时添加排序规则 */
		partcoll = partcollation->values[keyno];
		if (!attrsOnly && OidIsValid(partcoll) && partcoll != keycolcollation)
			appendStringInfo(&buf, " COLLATE %s",
							 generate_collation_name((partcoll)));

		/* 若非默认则添加操作符类名 */
		if (!attrsOnly)
			get_opclass_name(partclass->values[keyno], keycoltype, &buf);
	}

	if (!attrsOnly)
		appendStringInfoChar(&buf, ')');

	/* 清理 */
	ReleaseSysCache(tuple);

	return buf.data;
}

/*
 * pg_get_partition_constraintdef
 *
 * 以字符串形式返回输入关系的分区约束表达式
 */
Datum
pg_get_partition_constraintdef(PG_FUNCTION_ARGS)
{
	Oid			relationId = PG_GETARG_OID(0);
	Expr	   *constr_expr;
	int			prettyFlags;
	List	   *context;
	char	   *consrc;

	constr_expr = get_partition_qual_relid(relationId);

	/*如果没有分区约束则快速返回*/
	if (constr_expr == NULL)
		PG_RETURN_NULL();

	/*
	 * 反解析并返回约束表达式。
	 */
	prettyFlags = PRETTYFLAG_INDENT;
	context = deparse_context_for(get_relation_name(relationId), relationId);
	consrc = deparse_expression_pretty((Node *) constr_expr, context, false,
									   false, prettyFlags, 0);

	PG_RETURN_TEXT_P(string_to_text(consrc));
}

/*
 * pg_get_partconstrdef_string
 *
 * 以 C 字符串形式返回输入关系的分区约束，并带有给定
 * 的别名。不做美化打印。
 */
char *
pg_get_partconstrdef_string(Oid partitionId, char *aliasname)
{
	Expr	   *constr_expr;
	List	   *context;

	constr_expr = get_partition_qual_relid(partitionId);
	context = deparse_context_for(aliasname, partitionId);

	return deparse_expression((Node *) constr_expr, context, true, false);
}

/*
 * pg_get_constraintdef
 *
 * 返回该约束的定义，即所有需要出现在 "ALTER TABLE
 *  ... ADD CONSTRAINT <constraintname>" 之后的内容。
 */
Datum
pg_get_constraintdef(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);
	int			prettyFlags;
	char	   *res;

	prettyFlags = PRETTYFLAG_INDENT;

	res = pg_get_constraintdef_worker(constraintId, false, prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

Datum
pg_get_constraintdef_ext(PG_FUNCTION_ARGS)
{
	Oid			constraintId = PG_GETARG_OID(0);
	bool		pretty = PG_GETARG_BOOL(1);
	int			prettyFlags;
	char	   *res;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	res = pg_get_constraintdef_worker(constraintId, false, prettyFlags, true);

	if (res == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(string_to_text(res));
}

/*
 * 返回完整 "ALTER TABLE ... ADD CONSTRAINT" 命令的内部版本。
 */
char *
pg_get_constraintdef_command(Oid constraintId)
{
	return pg_get_constraintdef_worker(constraintId, true, 0, false);
}

/*
 * 从 9.4 起我们为此使用 MVCC 快照。
 */
static char *
pg_get_constraintdef_worker(Oid constraintId, bool fullCommand,
							int prettyFlags, bool missing_ok)
{
	HeapTuple	tup;
	Form_pg_constraint conForm;
	StringInfoData buf;
	SysScanDesc scandesc;
	ScanKeyData scankey[1];
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());
	Relation	relation = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&scankey[0],
				Anum_pg_constraint_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(constraintId));

	scandesc = systable_beginscan(relation,
								  ConstraintOidIndexId,
								  true,
								  snapshot,
								  1,
								  scankey);

	/*
	 * 我们稍后像经 SearchSysCache 获得那样，把该元组用于
	 * SysCacheGetAttr()，没有问题。
	 */
	tup = systable_getnext(scandesc);

	UnregisterSnapshot(snapshot);

	if (!HeapTupleIsValid(tup))
	{
		if (missing_ok)
		{
			systable_endscan(scandesc);
			table_close(relation, AccessShareLock);
			return NULL;
		}
		elog(ERROR, "could not find tuple for constraint %u", constraintId);
	}

	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	initStringInfo(&buf);

	if (fullCommand)
	{
		if (OidIsValid(conForm->conrelid))
		{
			/*
			 * 目前调用方希望 CHECK 约束用 ALTER TABLE（不带
			 * ONLY）。其余约束不继承，说不说均可。
			 * 将来可能需允许调用方指定命令中是否带 ONLY。
			 *
			 *
			 */
			appendStringInfo(&buf, "ALTER TABLE %s ADD CONSTRAINT %s ",
							 generate_qualified_relation_name(conForm->conrelid),
							 quote_identifier(NameStr(conForm->conname)));
		}
		else
		{
			/* 必须是域约束 */
			Assert(OidIsValid(conForm->contypid));
			appendStringInfo(&buf, "ALTER DOMAIN %s ADD CONSTRAINT %s ",
							 generate_qualified_type_name(conForm->contypid),
							 quote_identifier(NameStr(conForm->conname)));
		}
	}

	switch (conForm->contype)
	{
		case CONSTRAINT_FOREIGN:
			{
				Datum		val;
				bool		isnull;
				const char *string;

				/* 开始约束定义 */
				appendStringInfoString(&buf, "FOREIGN KEY (");

				/* 取得并构建引用列列表 */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conkey);

				decompile_column_index_array(val, conForm->conrelid, &buf);

				/* 加上外键关系名 */
				appendStringInfo(&buf, ") REFERENCES %s(",
								 generate_relation_name(conForm->confrelid,
														NIL));

				/* 取得并构建被引用列列表 */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_confkey);

				decompile_column_index_array(val, conForm->confrelid, &buf);

				appendStringInfoChar(&buf, ')');

				/* 加匹配类型*/
				switch (conForm->confmatchtype)
				{
					case FKCONSTR_MATCH_FULL:
						string = " MATCH FULL";
						break;
					case FKCONSTR_MATCH_PARTIAL:
						string = " MATCH PARTIAL";
						break;
					case FKCONSTR_MATCH_SIMPLE:
						string = "";
						break;
					default:
						elog(ERROR, "unrecognized confmatchtype: %d",
							 conForm->confmatchtype);
						string = "";	/* 让编译器安静 */
						break;
				}
				appendStringInfoString(&buf, string);

				/* 如需要则添加 ON UPDATE/ON DELETE 子句 */
				switch (conForm->confupdtype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* 抑制默认值 */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confupdtype: %d",
							 conForm->confupdtype);
						string = NULL;	/* 让编译器安静 */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON UPDATE %s", string);

				switch (conForm->confdeltype)
				{
					case FKCONSTR_ACTION_NOACTION:
						string = NULL;	/* 抑制默认值 */
						break;
					case FKCONSTR_ACTION_RESTRICT:
						string = "RESTRICT";
						break;
					case FKCONSTR_ACTION_CASCADE:
						string = "CASCADE";
						break;
					case FKCONSTR_ACTION_SETNULL:
						string = "SET NULL";
						break;
					case FKCONSTR_ACTION_SETDEFAULT:
						string = "SET DEFAULT";
						break;
					default:
						elog(ERROR, "unrecognized confdeltype: %d",
							 conForm->confdeltype);
						string = NULL;	/* 让编译器安静 */
						break;
				}
				if (string)
					appendStringInfo(&buf, " ON DELETE %s", string);

				/*
				 * 如指定列，为 SET NULL/SET DEFAULT 加列。
				 *
				 */
				val = SysCacheGetAttr(CONSTROID, tup,
									  Anum_pg_constraint_confdelsetcols, &isnull);
				if (!isnull)
				{
					appendStringInfoString(&buf, " (");
					decompile_column_index_array(val, conForm->conrelid, &buf);
					appendStringInfoChar(&buf, ')');
				}

				break;
			}
		case CONSTRAINT_PRIMARY:
		case CONSTRAINT_UNIQUE:
			{
				Datum		val;
				Oid			indexId;
				int			keyatts;
				HeapTuple	indtup;

				/* 开始约束定义 */
				if (conForm->contype == CONSTRAINT_PRIMARY)
					appendStringInfoString(&buf, "PRIMARY KEY ");
				else
					appendStringInfoString(&buf, "UNIQUE ");

				indexId = conForm->conindid;

				indtup = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(indexId));
				if (!HeapTupleIsValid(indtup))
					elog(ERROR, "cache lookup failed for index %u", indexId);
				if (conForm->contype == CONSTRAINT_UNIQUE &&
					((Form_pg_index) GETSTRUCT(indtup))->indnullsnotdistinct)
					appendStringInfoString(&buf, "NULLS NOT DISTINCT ");

				appendStringInfoChar(&buf, '(');

				/* 取得并构建目标列列表 */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conkey);

				keyatts = decompile_column_index_array(val, conForm->conrelid, &buf);

				appendStringInfoChar(&buf, ')');

				/* 构建包含列列表（来自 pg_index.indkeys） */
				val = SysCacheGetAttrNotNull(INDEXRELID, indtup,
											 Anum_pg_index_indnatts);
				if (DatumGetInt32(val) > keyatts)
				{
					Datum		cols;
					Datum	   *keys;
					int			nKeys;
					int			j;

					appendStringInfoString(&buf, " INCLUDE (");

					cols = SysCacheGetAttrNotNull(INDEXRELID, indtup,
												  Anum_pg_index_indkey);

					deconstruct_array_builtin(DatumGetArrayTypeP(cols), INT2OID,
											  &keys, NULL, &nKeys);

					for (j = keyatts; j < nKeys; j++)
					{
						char	   *colName;

						colName = get_attname(conForm->conrelid,
											  DatumGetInt16(keys[j]), false);
						if (j > keyatts)
							appendStringInfoString(&buf, ", ");
						appendStringInfoString(&buf, quote_identifier(colName));
					}

					appendStringInfoChar(&buf, ')');
				}
				ReleaseSysCache(indtup);

				/*XXX 为什么仅在 fullCommand 时打印这些位？*/
				if (fullCommand && OidIsValid(indexId))
				{
					char	   *options = flatten_reloptions(indexId);
					Oid			tblspc;

					if (options)
					{
						appendStringInfo(&buf, " WITH (%s)", options);
						pfree(options);
					}

					/*
					 * 打印表空间，除非它就是数据库默认值。
					 * ALTER TABLE 用法需要此行为来准确重现
					 * 目录状态。
					 *
					 */
					tblspc = get_rel_tablespace(indexId);
					if (OidIsValid(tblspc))
						appendStringInfo(&buf, " USING INDEX TABLESPACE %s",
										 quote_identifier(get_tablespace_name(tblspc)));
				}

				break;
			}
		case CONSTRAINT_CHECK:
			{
				Datum		val;
				char	   *conbin;
				char	   *consrc;
				Node	   *expr;
				List	   *context;

				/* 以解析树形式取得约束表达式 */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conbin);

				conbin = TextDatumGetCString(val);
				expr = stringToNode(conbin);

				/* 为约束中的 Var 节点建立反解析上下文 */
				if (conForm->conrelid != InvalidOid)
				{
					/* 关系约束 */
					context = deparse_context_for(get_relation_name(conForm->conrelid),
												  conForm->conrelid);
				}
				else
				{
					/* 域约束 --- 不可能有 Var */
					context = NIL;
				}

				consrc = deparse_expression_pretty(expr, context, false, false,
												   prettyFlags, 0);

				/*
				 * 现在输出约束定义，必要时加上 NO INHERIT。
				 *
				 *
				 * 有时表达式完全括起，无需外层括号……
				 * 但也有些情况确实需要它们。
				 * 目前采取保守做法。
				 *
				 *
				 * 注意，检查前导 "(" 与尾随 ")" 是不够的，
				 * 例如 "(x > 0) AND (y > 0)" 这种情况。
				 */
				appendStringInfo(&buf, "CHECK (%s)%s",
								 consrc,
								 conForm->connoinherit ? " NO INHERIT" : "");
				break;
			}
		case CONSTRAINT_TRIGGER:

			/*
			 * 用户定义约束触发器没有 ALTER TABLE 语法，
			 * 但打印点什么总比抛错好；若抛错本函数就无法
			 * 安全地应用于 pg_constraint 的全部行。
			 *
			 */
			appendStringInfoString(&buf, "TRIGGER");
			break;
		case CONSTRAINT_EXCLUSION:
			{
				Oid			indexOid = conForm->conindid;
				Datum		val;
				Datum	   *elems;
				int			nElems;
				int			i;
				Oid		   *operators;

				/* 从 pg_constraint 元组中提取操作符 OID */
				val = SysCacheGetAttrNotNull(CONSTROID, tup,
											 Anum_pg_constraint_conexclop);

				deconstruct_array_builtin(DatumGetArrayTypeP(val), OIDOID,
										  &elems, NULL, &nElems);

				operators = (Oid *) palloc(nElems * sizeof(Oid));
				for (i = 0; i < nElems; i++)
					operators[i] = DatumGetObjectId(elems[i]);

				/* pg_get_indexdef_worker 完成其余 */
				/* 按 pg_dump 的要求省略表空间 */
				appendStringInfoString(&buf,
									   pg_get_indexdef_worker(indexOid,
															  0,
															  operators,
															  false,
															  false,
															  false,
															  false,
															  prettyFlags,
															  false));
				break;
			}
		default:
			elog(ERROR, "invalid constraint type \"%c\"", conForm->contype);
			break;
	}

	if (conForm->condeferrable)
		appendStringInfoString(&buf, " DEFERRABLE");
	if (conForm->condeferred)
		appendStringInfoString(&buf, " INITIALLY DEFERRED");
	if (!conForm->convalidated)
		appendStringInfoString(&buf, " NOT VALID");

	/* 清理 */
	systable_endscan(scandesc);
	table_close(relation, AccessShareLock);

	return buf.data;
}


/*
 * 把 int16[] Datum 转换成关系列名的逗号分隔列表；
 * 把该列表追加到 buf。返回键的数目。
 *
 */
static int
decompile_column_index_array(Datum column_index_array, Oid relId,
							 StringInfo buf)
{
	Datum	   *keys;
	int			nKeys;
	int			j;

	/* 从 int16 数组中提取数据 */
	deconstruct_array_builtin(DatumGetArrayTypeP(column_index_array), INT2OID,
							  &keys, NULL, &nKeys);

	for (j = 0; j < nKeys; j++)
	{
		char	   *colName;

		colName = get_attname(relId, DatumGetInt16(keys[j]), false);

		if (j == 0)
			appendStringInfoString(buf, quote_identifier(colName));
		else
			appendStringInfo(buf, ", %s", quote_identifier(colName));
	}

	return nKeys;
}


/* ----------
 * pg_get_expr			- 反解析表达式树
 *
 * 输入：nodeToString 形式的表达式树和一个关系 OID
 *
 * 输出：反解析的表达式
 *
 * 表达式目前只能引用单个关系，即第二个参数指定的
 * 该关系。这对部分索引、列默认表达式等足够。
 * 也支持无 Var 的表达式，此时 OID 可为 InvalidOid。
 *
 *
 * 若 OID 非零但实际无效，不要抛错，只返回 NULL。
 * 这值得商榷，但它是历史做法，且有助于在检查
 * 刚删除关系的目录项时避免不想要的失败。
 *
 *
 * 我们期望本函数对任何出现在目录 pg_node_tree 列中
 * 的节点树都能工作或抛出较干净的错误。查询树，
 * 例如 pg_rewrite.ev_action 中出现的那些，不受支持。
 * 多个关系中的表达式也不支持，它们可能出现在
 * pg_rewrite.ev_qual 之类的地方。
 * ----------
 */
Datum
pg_get_expr(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_PP(0);
	Oid			relid = PG_GETARG_OID(1);
	text	   *result;
	int			prettyFlags;

	prettyFlags = PRETTYFLAG_INDENT;

	result = pg_get_expr_worker(expr, relid, prettyFlags);
	if (result)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

Datum
pg_get_expr_ext(PG_FUNCTION_ARGS)
{
	text	   *expr = PG_GETARG_TEXT_PP(0);
	Oid			relid = PG_GETARG_OID(1);
	bool		pretty = PG_GETARG_BOOL(2);
	text	   *result;
	int			prettyFlags;

	prettyFlags = GET_PRETTY_FLAGS(pretty);

	result = pg_get_expr_worker(expr, relid, prettyFlags);
	if (result)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

static text *
pg_get_expr_worker(text *expr, Oid relid, int prettyFlags)
{
	Node	   *node;
	Node	   *tst;
	Relids		relids;
	List	   *context;
	char	   *exprstr;
	Relation	rel = NULL;
	char	   *str;

	/* 把 pg_node_tree（实为 TEXT）输入转换成 C 字符串*/
	exprstr = text_to_cstring(expr);

	/* 把表达式转换为节点树 */
	node = (Node *) stringToNode(exprstr);

	pfree(exprstr);

	/*
	 * 若输入是查询树而非表达式树则抛错。虽然本可以
	 * 支持查询，但似乎没有充分理由这么做。在大多数
	 * 这类目录列中我们会看到 Query 节点的列表，甚至嵌套
	 * 的列表，因此检查前要先下钻到非列表节点。
	 */
	tst = node;
	while (tst && IsA(tst, List))
		tst = linitial((List *) tst);
	if (tst && IsA(tst, Query))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("input is a query, not an expression")));

	/*
	 * 若表达式含有我们无法反解析的 Var，则抛错。
	 *
	 */
	relids = pull_varnos(NULL, node);
	if (OidIsValid(relid))
	{
		if (!bms_is_subset(relids, bms_make_singleton(1)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("expression contains variables of more than one relation")));
	}
	else
	{
		if (!bms_is_empty(relids))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("expression contains variables")));
	}

	/*
	 * 如有需要则准备反解析上下文。若带 relid 反解析，
	 * 就需要临时打开并锁定该关系，确保它不会在
	 * 我们操作期间消失。（set_relation_column_names 反正
	 * 也会锁定它，故并未引入新行为。）
	 */
	if (OidIsValid(relid))
	{
		rel = try_relation_open(relid, AccessShareLock);
		if (rel == NULL)
			return NULL;
		context = deparse_context_for(RelationGetRelationName(rel), relid);
	}
	else
		context = NIL;

	/*反解析*/
	str = deparse_expression_pretty(node, context, false, false,
									prettyFlags, 0);

	if (rel != NULL)
		relation_close(rel, AccessShareLock);

	return string_to_text(str);
}


/* ----------
 * pg_get_userbyid		- 按角色 ID 取用户名，
 *					  回退为 'unknown (OID=n)'
 * ----------
 */
Datum
pg_get_userbyid(PG_FUNCTION_ARGS)
{
	Oid			roleid = PG_GETARG_OID(0);
	Name		result;
	HeapTuple	roletup;
	Form_pg_authid role_rec;

	/*
	 * 为结果分配空间
	 */
	result = (Name) palloc(NAMEDATALEN);
	memset(NameStr(*result), 0, NAMEDATALEN);

	/*
	 * 取得 pg_authid 项并打印结果
	 */
	roletup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
	if (HeapTupleIsValid(roletup))
	{
		role_rec = (Form_pg_authid) GETSTRUCT(roletup);
		*result = role_rec->rolname;
		ReleaseSysCache(roletup);
	}
	else
		sprintf(NameStr(*result), "unknown (OID=%u)", roleid);

	PG_RETURN_NAME(result);
}


/*
 * pg_get_serial_sequence
 *			取得身份列或 serial 列所用序列的名称，格式适
 *			合传给 setval、nextval 或 currval。第一参数不作
 *			双引号处理，第二参数则要 —— 缘由见文档。
 *
 */
Datum
pg_get_serial_sequence(PG_FUNCTION_ARGS)
{
	text	   *tablename = PG_GETARG_TEXT_PP(0);
	text	   *columnname = PG_GETARG_TEXT_PP(1);
	RangeVar   *tablerv;
	Oid			tableOid;
	char	   *column;
	AttrNumber	attnum;
	Oid			sequenceId = InvalidOid;
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	/* 查找表名。不能加锁 —— 我们可能没有权限。 */
	tablerv = makeRangeVarFromNameList(textToQualifiedNameList(tablename));
	tableOid = RangeVarGetRelid(tablerv, NoLock, false);

	/* 取得该列的编号 */
	column = text_to_cstring(columnname);

	attnum = get_attnum(tableOid, column);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						column, tablerv->relname)));

	/* 在依赖表中搜索依赖的序列 */
	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tableOid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_refobjsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend deprec = (Form_pg_depend) GETSTRUCT(tup);

		/*
		 * 查找序列对列的自动依赖（serial 列）或内部依赖
		 * （身份列）。（需要 relkind 测试，索引也可能有
		 * 对列的自动依赖。）
		 */
		if (deprec->classid == RelationRelationId &&
			deprec->objsubid == 0 &&
			(deprec->deptype == DEPENDENCY_AUTO ||
			 deprec->deptype == DEPENDENCY_INTERNAL) &&
			get_rel_relkind(deprec->objid) == RELKIND_SEQUENCE)
		{
			sequenceId = deprec->objid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	if (OidIsValid(sequenceId))
	{
		char	   *result;

		result = generate_qualified_relation_name(sequenceId);

		PG_RETURN_TEXT_P(string_to_text(result));
	}

	PG_RETURN_NULL();
}


/*
 * pg_get_functiondef
 *			返回指定函数的完整 "CREATE OR REPLACE FUNCTION ..."
 *			语句。
 *
 * 注意：如果你改变本函数的输出格式，当心不要破坏
 * psql 的规则（\ef 与 \sf 中识别函数体开头的规则）。
 * 就是说：函数体所在的行以 "AS "、"BEGIN " 或 "RETURN "
 * 开头，而之前的任何行都不会看起来像那样。
 */
Datum
pg_get_functiondef(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	StringInfoData dq;
	HeapTuple	proctup;
	Form_pg_proc proc;
	bool		isfunction;
	Datum		tmp;
	bool		isnull;
	const char *prosrc;
	const char *name;
	const char *nsp;
	float4		procost;
	int			oldlen;

	initStringInfo(&buf);

	/* 查找该函数 */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	proc = (Form_pg_proc) GETSTRUCT(proctup);
	name = NameStr(proc->proname);

	if (proc->prokind == PROKIND_AGGREGATE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is an aggregate function", name)));

	isfunction = (proc->prokind != PROKIND_PROCEDURE);

	/*
	 * 我们总是给函数名加模式限定，确保替换的是正确的
	 * 函数。
	 */
	nsp = get_namespace_name_or_temp(proc->pronamespace);
	appendStringInfo(&buf, "CREATE OR REPLACE %s %s(",
					 isfunction ? "FUNCTION" : "PROCEDURE",
					 quote_qualified_identifier(nsp, name));
	(void) print_function_arguments(&buf, proctup, false, true);
	appendStringInfoString(&buf, ")\n");
	if (isfunction)
	{
		appendStringInfoString(&buf, " RETURNS ");
		print_function_rettype(&buf, proctup);
		appendStringInfoChar(&buf, '\n');
	}

	print_function_trftypes(&buf, proctup);

	appendStringInfo(&buf, " LANGUAGE %s\n",
					 quote_identifier(get_language_name(proc->prolang, false)));

	/* 在一行上输出若干杂项选项 */
	oldlen = buf.len;

	if (proc->prokind == PROKIND_WINDOW)
		appendStringInfoString(&buf, " WINDOW");
	switch (proc->provolatile)
	{
		case PROVOLATILE_IMMUTABLE:
			appendStringInfoString(&buf, " IMMUTABLE");
			break;
		case PROVOLATILE_STABLE:
			appendStringInfoString(&buf, " STABLE");
			break;
		case PROVOLATILE_VOLATILE:
			break;
	}

	switch (proc->proparallel)
	{
		case PROPARALLEL_SAFE:
			appendStringInfoString(&buf, " PARALLEL SAFE");
			break;
		case PROPARALLEL_RESTRICTED:
			appendStringInfoString(&buf, " PARALLEL RESTRICTED");
			break;
		case PROPARALLEL_UNSAFE:
			break;
	}

	if (proc->proisstrict)
		appendStringInfoString(&buf, " STRICT");
	if (proc->prosecdef)
		appendStringInfoString(&buf, " SECURITY DEFINER");
	if (proc->proleakproof)
		appendStringInfoString(&buf, " LEAKPROOF");

	/* 此处默认开销与行数的代码应与 functioncmds.c 一致 */
	if (proc->prolang == INTERNALlanguageId ||
		proc->prolang == ClanguageId)
		procost = 1;
	else
		procost = 100;
	if (proc->procost != procost)
		appendStringInfo(&buf, " COST %g", proc->procost);

	if (proc->prorows > 0 && proc->prorows != 1000)
		appendStringInfo(&buf, " ROWS %g", proc->prorows);

	if (proc->prosupport)
	{
		Oid			argtypes[1];

		/*
		 * 若支持函数名在当前搜索路径中无法解析，
		 * 就应给它加上模式限定。
		 */
		argtypes[0] = INTERNALOID;
		appendStringInfo(&buf, " SUPPORT %s",
						 generate_function_name(proc->prosupport, 1,
												NIL, argtypes,
												false, NULL, false));
	}

	if (oldlen != buf.len)
		appendStringInfoChar(&buf, '\n');

	/* 每行输出一个 proconfig 选项 */
	tmp = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proconfig, &isnull);
	if (!isnull)
	{
		ArrayType  *a = DatumGetArrayTypeP(tmp);
		int			i;

		Assert(ARR_ELEMTYPE(a) == TEXTOID);
		Assert(ARR_NDIM(a) == 1);
		Assert(ARR_LBOUND(a)[0] == 1);

		for (i = 1; i <= ARR_DIMS(a)[0]; i++)
		{
			Datum		d;

			d = array_ref(a, 1, &i,
						  -1 /* 变长数组*/ ,
						  -1 /*TEXT 的 typlen*/ ,
						  false /*TEXT 的 typbyval*/ ,
						  TYPALIGN_INT /*TEXT 的 typalign*/ ,
						  &isnull);
			if (!isnull)
			{
				char	   *configitem = TextDatumGetCString(d);
				char	   *pos;

				pos = strchr(configitem, '=');
				if (pos == NULL)
					continue;
				*pos++ = '\0';

				appendStringInfo(&buf, " SET %s TO ",
								 quote_identifier(configitem));

				/*
				 * 标为 GUC_LIST_QUOTE 的变量放入 proconfig 数组前
				 * 已被 flatten_set_variable_args() 完全加了引号。
				 * 但那里的引号规则不同于 SQL，须拆开值，
				 * 再给元素按字面量加引号。（元素或
				 * 已带双引号，但不能径直交给 SQL 解析器，
				 * 否则零长度或超 NAMEDATALEN 的元素会出错。）
				 *
				 *
				 *
				 *
				 * 未标记变量则应直接按简单字面量输出。
				 * 若 guc.c 不认识该变量就会这么做；这使
				 * 扩展变量用 GUC_LIST_QUOTE 便不安全。
				 *
				 */
				if (GetConfigOptionFlags(configitem, true) & GUC_LIST_QUOTE)
				{
					List	   *namelist;
					ListCell   *lc;

					/* 把字符串解析成标识符列表 */
					if (!SplitGUCList(pos, ',', &namelist))
					{
						/* 这实际上不应该失败*/
						elog(ERROR, "invalid list syntax in proconfig item");
					}
					foreach(lc, namelist)
					{
						char	   *curname = (char *) lfirst(lc);

						simple_quote_literal(&buf, curname);
						if (lnext(namelist, lc))
							appendStringInfoString(&buf, ", ");
					}
				}
				else
					simple_quote_literal(&buf, pos);
				appendStringInfoChar(&buf, '\n');
			}
		}
	}

	/* 最后是函数定义…… */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosqlbody, &isnull);
	if (proc->prolang == SQLlanguageId && !isnull)
	{
		print_function_sqlbody(&buf, proctup);
	}
	else
	{
		appendStringInfoString(&buf, "AS ");

		tmp = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_probin, &isnull);
		if (!isnull)
		{
			simple_quote_literal(&buf, TextDatumGetCString(tmp));
			appendStringInfoString(&buf, ", "); /* 假定 prosrc 非空 */
		}

		tmp = SysCacheGetAttrNotNull(PROCOID, proctup, Anum_pg_proc_prosrc);
		prosrc = TextDatumGetCString(tmp);

		/*
		 * 我们总用美元引号。需要想出合适的定界符。
		 *
		 * 由于用户很可能编辑函数体字符串，不应使用他
		 * 很容易就能制造冲突的短定界符。因此首选
		 * "$function$"/"$procedure$"，但必要时加以扩展。
		 *
		 */
		initStringInfo(&dq);
		appendStringInfoChar(&dq, '$');
		appendStringInfoString(&dq, (isfunction ? "function" : "procedure"));
		while (strstr(prosrc, dq.data) != NULL)
			appendStringInfoChar(&dq, 'x');
		appendStringInfoChar(&dq, '$');

		appendBinaryStringInfo(&buf, dq.data, dq.len);
		appendStringInfoString(&buf, prosrc);
		appendBinaryStringInfo(&buf, dq.data, dq.len);
	}

	appendStringInfoChar(&buf, '\n');

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_arguments
 *			取得某函数参数的格式良好列表。
 *			这就是在 CREATE FUNCTION 中出现在括号之间的
 *	全部内容。
 */
Datum
pg_get_function_arguments(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	initStringInfo(&buf);

	(void) print_function_arguments(&buf, proctup, false, true);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_identity_arguments
 *			取得某函数参数的格式化列表。
 *			这就是在 ALTER FUNCTION 等语句中出现在括号之间的
 *	全部内容。特别不打印默认值。
 */
Datum
pg_get_function_identity_arguments(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	initStringInfo(&buf);

	(void) print_function_arguments(&buf, proctup, false, false);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_result
 *			取得函数结果类型的格式良好版本。
 *			即 CREATE FUNCTION 中 RETURNS 之后的内容。
 */
Datum
pg_get_function_result(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	if (((Form_pg_proc) GETSTRUCT(proctup))->prokind == PROKIND_PROCEDURE)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	initStringInfo(&buf);

	print_function_rettype(&buf, proctup);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(buf.data));
}

/*
 * pg_get_function_result 的主体：把函数的返回类型
 * 加到该缓冲区。
 */
static void
print_function_rettype(StringInfo buf, HeapTuple proctup)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	int			ntabargs = 0;
	StringInfoData rbuf;

	initStringInfo(&rbuf);

	if (proc->proretset)
	{
		/* 它可能是表函数；尝试打印参数 */
		appendStringInfoString(&rbuf, "TABLE(");
		ntabargs = print_function_arguments(&rbuf, proctup, true, false);
		if (ntabargs > 0)
			appendStringInfoChar(&rbuf, ')');
		else
			resetStringInfo(&rbuf);
	}

	if (ntabargs == 0)
	{
		/* 不是表函数，按常规处理 */
		if (proc->proretset)
			appendStringInfoString(&rbuf, "SETOF ");
		appendStringInfoString(&rbuf, format_type_be(proc->prorettype));
	}

	appendBinaryStringInfo(buf, rbuf.data, rbuf.len);
}

/*
 * pg_get_function_arguments 与 pg_get_function_result 的
 * 公共代码：把所需的参数子集追加到 buf。当
 * print_table_args 为真时只打印 TABLE 参数，为假时打印
 * 其余参数。print_defaults 为真才打印默认值。
 * 函数返回值为已打印参数的数目。
 */
static int
print_function_arguments(StringInfo buf, HeapTuple proctup,
						 bool print_table_args, bool print_defaults)
{
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proctup);
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			insertorderbyat = -1;
	int			argsprinted;
	int			inputargno;
	int			nlackdefaults;
	List	   *argdefaults = NIL;
	ListCell   *nextargdefault = NULL;
	int			i;

	numargs = get_func_arg_info(proctup,
								&argtypes, &argnames, &argmodes);

	nlackdefaults = numargs;
	if (print_defaults && proc->pronargdefaults > 0)
	{
		Datum		proargdefaults;
		bool		isnull;

		proargdefaults = SysCacheGetAttr(PROCOID, proctup,
										 Anum_pg_proc_proargdefaults,
										 &isnull);
		if (!isnull)
		{
			char	   *str;

			str = TextDatumGetCString(proargdefaults);
			argdefaults = castNode(List, stringToNode(str));
			pfree(str);
			nextargdefault = list_head(argdefaults);
			/* nlackdefaults 只统计缺少默认值的*输入*参数 */
			nlackdefaults = proc->pronargs - list_length(argdefaults);
		}
	}

	/* 检查有序集聚合是否需要特殊处理 */
	if (proc->prokind == PROKIND_AGGREGATE)
	{
		HeapTuple	aggtup;
		Form_pg_aggregate agg;

		aggtup = SearchSysCache1(AGGFNOID, proc->oid);
		if (!HeapTupleIsValid(aggtup))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 proc->oid);
		agg = (Form_pg_aggregate) GETSTRUCT(aggtup);
		if (AGGKIND_IS_ORDERED_SET(agg->aggkind))
			insertorderbyat = agg->aggnumdirectargs;
		ReleaseSysCache(aggtup);
	}

	argsprinted = 0;
	inputargno = 0;
	for (i = 0; i < numargs; i++)
	{
		Oid			argtype = argtypes[i];
		char	   *argname = argnames ? argnames[i] : NULL;
		char		argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
		const char *modename;
		bool		isinput;

		switch (argmode)
		{
			case PROARGMODE_IN:

				/*
				 * 过程要显式标记所有参数模式，以避免与
				 * DROP PROCEDURE 的 SQL 语法产生歧义。
				 */
				if (proc->prokind == PROKIND_PROCEDURE)
					modename = "IN ";
				else
					modename = "";
				isinput = true;
				break;
			case PROARGMODE_INOUT:
				modename = "INOUT ";
				isinput = true;
				break;
			case PROARGMODE_OUT:
				modename = "OUT ";
				isinput = false;
				break;
			case PROARGMODE_VARIADIC:
				modename = "VARIADIC ";
				isinput = true;
				break;
			case PROARGMODE_TABLE:
				modename = "";
				isinput = false;
				break;
			default:
				elog(ERROR, "invalid parameter mode '%c'", argmode);
				modename = NULL;	/* 让编译器安静 */
				isinput = false;
				break;
		}
		if (isinput)
			inputargno++;		/* 以 1 为基的计数器 */

		if (print_table_args != (argmode == PROARGMODE_TABLE))
			continue;

		if (argsprinted == insertorderbyat)
		{
			if (argsprinted)
				appendStringInfoChar(buf, ' ');
			appendStringInfoString(buf, "ORDER BY ");
		}
		else if (argsprinted)
			appendStringInfoString(buf, ", ");

		appendStringInfoString(buf, modename);
		if (argname && argname[0])
			appendStringInfo(buf, "%s ", quote_identifier(argname));
		appendStringInfoString(buf, format_type_be(argtype));
		if (print_defaults && isinput && inputargno > nlackdefaults)
		{
			Node	   *expr;

			Assert(nextargdefault != NULL);
			expr = (Node *) lfirst(nextargdefault);
			nextargdefault = lnext(argdefaults, nextargdefault);

			appendStringInfo(buf, " DEFAULT %s",
							 deparse_expression(expr, NIL, false, false));
		}
		argsprinted++;

		/* 取巧：对变参有序集聚合，最后一个参数打印两次*/
		if (argsprinted == insertorderbyat && i == numargs - 1)
		{
			i--;
			/* 聚合本不该有默认值，但以防万一…… */
			print_defaults = false;
		}
	}

	return argsprinted;
}

static bool
is_input_argument(int nth, const char *argmodes)
{
	return (!argmodes
			|| argmodes[nth] == PROARGMODE_IN
			|| argmodes[nth] == PROARGMODE_INOUT
			|| argmodes[nth] == PROARGMODE_VARIADIC);
}

/*
 * 把用到的转换类型追加到指定缓冲区
 */
static void
print_function_trftypes(StringInfo buf, HeapTuple proctup)
{
	Oid		   *trftypes;
	int			ntypes;

	ntypes = get_func_trftypes(proctup, &trftypes);
	if (ntypes > 0)
	{
		int			i;

		appendStringInfoString(buf, " TRANSFORM ");
		for (i = 0; i < ntypes; i++)
		{
			if (i != 0)
				appendStringInfoString(buf, ", ");
			appendStringInfo(buf, "FOR TYPE %s", format_type_be(trftypes[i]));
		}
		appendStringInfoChar(buf, '\n');
	}
}

/*
 * 取得函数参数默认值的文本表示。本函数第二参数
 * 是全部参数中的参数序号（即 proallargtypes，*不是*
 * proargtypes），从 1 开始计，因为 information_schema.sql
 * 就是这样使用它的。
 */
Datum
pg_get_function_arg_default(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	int32		nth_arg = PG_GETARG_INT32(1);
	HeapTuple	proctup;
	Form_pg_proc proc;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	int			i;
	List	   *argdefaults;
	Node	   *node;
	char	   *str;
	int			nth_inputarg;
	Datum		proargdefaults;
	bool		isnull;
	int			nth_default;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	numargs = get_func_arg_info(proctup, &argtypes, &argnames, &argmodes);
	if (nth_arg < 1 || nth_arg > numargs || !is_input_argument(nth_arg - 1, argmodes))
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	nth_inputarg = 0;
	for (i = 0; i < nth_arg; i++)
		if (is_input_argument(i, argmodes))
			nth_inputarg++;

	proargdefaults = SysCacheGetAttr(PROCOID, proctup,
									 Anum_pg_proc_proargdefaults,
									 &isnull);
	if (isnull)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	str = TextDatumGetCString(proargdefaults);
	argdefaults = castNode(List, stringToNode(str));
	pfree(str);

	proc = (Form_pg_proc) GETSTRUCT(proctup);

	/*
	 * 计算 proargdefaults 中的下标：proargdefaults 对应于
	 * 最后 N 个输入参数，N = pronargdefaults。
	 */
	nth_default = nth_inputarg - 1 - (proc->pronargs - proc->pronargdefaults);

	if (nth_default < 0 || nth_default >= list_length(argdefaults))
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}
	node = list_nth(argdefaults, nth_default);
	str = deparse_expression(node, NIL, false, false);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(string_to_text(str));
}

static void
print_function_sqlbody(StringInfo buf, HeapTuple proctup)
{
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	deparse_namespace dpns = {0};
	Datum		tmp;
	Node	   *n;

	dpns.funcname = pstrdup(NameStr(((Form_pg_proc) GETSTRUCT(proctup))->proname));
	numargs = get_func_arg_info(proctup,
								&argtypes, &argnames, &argmodes);
	dpns.numargs = numargs;
	dpns.argnames = argnames;

	tmp = SysCacheGetAttrNotNull(PROCOID, proctup, Anum_pg_proc_prosqlbody);
	n = stringToNode(TextDatumGetCString(tmp));

	if (IsA(n, List))
	{
		List	   *stmts;
		ListCell   *lc;

		stmts = linitial(castNode(List, n));

		appendStringInfoString(buf, "BEGIN ATOMIC\n");

		foreach(lc, stmts)
		{
			Query	   *query = lfirst_node(Query, lc);

			/* 看来宜对关系至少取得 AccessShareLock */
			AcquireRewriteLocks(query, false, false);
			get_query_def(query, buf, list_make1(&dpns), NULL, false,
						  PRETTYFLAG_INDENT, WRAP_COLUMN_DEFAULT, 1);
			appendStringInfoChar(buf, ';');
			appendStringInfoChar(buf, '\n');
		}

		appendStringInfoString(buf, "END");
	}
	else
	{
		Query	   *query = castNode(Query, n);

		/* 看来宜对关系至少取得 AccessShareLock */
		AcquireRewriteLocks(query, false, false);
		get_query_def(query, buf, list_make1(&dpns), NULL, false,
					  0, WRAP_COLUMN_DEFAULT, 0);
	}
}

Datum
pg_get_function_sqlbody(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	StringInfoData buf;
	HeapTuple	proctup;
	bool		isnull;

	initStringInfo(&buf);

	/* 查找该函数 */
	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		PG_RETURN_NULL();

	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosqlbody, &isnull);
	if (isnull)
	{
		ReleaseSysCache(proctup);
		PG_RETURN_NULL();
	}

	print_function_sqlbody(&buf, proctup);

	ReleaseSysCache(proctup);

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}


/*
 * deparse_expression			- 反解析表达式的通用工具
 *
 * 调用 deparse_expression_pretty 并禁用全部美化打印
 */
char *
deparse_expression(Node *expr, List *dpcontext,
				   bool forceprefix, bool showimplicit)
{
	return deparse_expression_pretty(expr, dpcontext, forceprefix,
									 showimplicit, 0, 0);
}

/* ----------
 * deparse_expression_pretty	- 反解析表达式的通用工具
 *
 * expr 是要反解析的节点树。它必须是已转换的表达式
 * 树（非 gram.y 原始输出）。
 *
 * dpcontext 是 deparse_namespace 节点的列表，表示解释
 * 节点树中 Var 的上下文。如果不期望有 Var，可以为
 * NIL。
 *
 * forceprefix 为真强制所有 Var 都带上其表名前缀。
 *
 * showimplicit 为真强制显示所有隐式转换。
 *
 * 尝试按 prettyFlags 与 startIndent 美化输出。
 *
 * 结果是 palloc 的字符串。
 * ----------
 */
static char *
deparse_expression_pretty(Node *expr, List *dpcontext,
						  bool forceprefix, bool showimplicit,
						  int prettyFlags, int startIndent)
{
	StringInfoData buf;
	deparse_context context;

	initStringInfo(&buf);
	context.buf = &buf;
	context.namespaces = dpcontext;
	context.resultDesc = NULL;
	context.targetList = NIL;
	context.windowClause = NIL;
	context.varprefix = forceprefix;
	context.prettyFlags = prettyFlags;
	context.wrapColumn = WRAP_COLUMN_DEFAULT;
	context.indentLevel = startIndent;
	context.colNamesVisible = true;
	context.inGroupBy = false;
	context.varInOrderBy = false;
	context.appendparents = NULL;

	get_rule_expr(expr, &context, showimplicit);

	return buf.data;
}

/* ----------
 * deparse_context_for			- 为单个关系构建反解析上下文
 *
 * 给定关系的引用名（别名）与 OID，为只引用该关系（
 * varno 为 1、varlevelsup 为 0）的表达式构建反解析
 * 上下文。这对 deparse_expression 的许多用途已经足够。
 * ----------
 */
List *
deparse_context_for(const char *aliasname, Oid relid)
{
	deparse_namespace *dpns;
	RangeTblEntry *rte;

	dpns = (deparse_namespace *) palloc0(sizeof(deparse_namespace));

	/* 为该关系构建最小的 RTE */
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = relid;
	rte->relkind = RELKIND_RELATION;	/* 此处无需精确 */
	rte->rellockmode = AccessShareLock;
	rte->alias = makeAlias(aliasname, NIL);
	rte->eref = rte->alias;
	rte->lateral = false;
	rte->inh = false;
	rte->inFromCl = true;

	/* 构建单元素范围表 */
	dpns->rtable = list_make1(rte);
	dpns->subplans = NIL;
	dpns->ctes = NIL;
	dpns->appendrels = NULL;
	set_rtable_names(dpns, NIL, NULL);
	set_simple_column_names(dpns);

	/* 返回单层命名空间栈 */
	return list_make1(dpns);
}

/*
 * deparse_context_for_plan_tree - 为 plan 树构建反解析上下文
 *
 * 在 plan 树中反解析表达式时，我们用 plan 的范围表来
 * 解析简单 Var 的名字。若范围表很大，为其初始化列名
 * 相当昂贵，而且对 plan 树中的每个表达式来说都一样；
 * 因此只做一次，并为每个表达式重用本函数结果。
 * （注意：该结果在应用 set_deparse_context_plan() 之前，
 * 尚不可用。）
 *
 * 除了 PlannedStmt 之外，还要传入先前调用
 * select_rtable_names_for_explain 时分配的各 RTE 别名。
 */
List *
deparse_context_for_plan_tree(PlannedStmt *pstmt, List *rtable_names)
{
	deparse_namespace *dpns;

	dpns = (deparse_namespace *) palloc0(sizeof(deparse_namespace));

	/* 初始化在整个 plan 树中保持不变的字段 */
	dpns->rtable = pstmt->rtable;
	dpns->rtable_names = rtable_names;
	dpns->subplans = pstmt->subplans;
	dpns->ctes = NIL;
	if (pstmt->appendRelations)
	{
		/* 建立数组，按子 relid 索引 */
		int			ntables = list_length(dpns->rtable);
		ListCell   *lc;

		dpns->appendrels = (AppendRelInfo **)
			palloc0((ntables + 1) * sizeof(AppendRelInfo *));
		foreach(lc, pstmt->appendRelations)
		{
			AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
			Index		crelid = appinfo->child_relid;

			Assert(crelid > 0 && crelid <= ntables);
			Assert(dpns->appendrels[crelid] == NULL);
			dpns->appendrels[crelid] = appinfo;
		}
	}
	else
		dpns->appendrels = NULL;	/* 不需要它 */

	/*
	 * 建立列名别名。对连接 RTE 会得到相当失真的结果，
	 * 但这没有关系，因为 plan 树不含任何连接别名 Var。
	 *
	 */
	set_simple_column_names(dpns);

	/* 返回单层命名空间栈 */
	return list_make1(dpns);
}

/*
 * set_deparse_context_plan - 指定包含表达式的 plan 节点
 *
 * 在 plan 树中反解析表达式时，我们可能不得不解析
 * OUTER_VAR、INNER_VAR 或 INDEX_VAR 引用。为此，调用
 * 方必须提供父 Plan 节点。这样 OUTER_VAR 与 INNER_VAR
 * 引用便可通过深入左、右子 plan 来解析。类似地，
 * INDEX_VAR 引用可以通过引用父 IndexOnlyScan 节点给出
 * 的 indextlist，或 ForeignScan 与 CustomScan 节点中的
 * 扫描目标列表来解析。（注意，我们目前不支持对常规
 * IndexScan 或 BitmapIndexScan 节点中的 indexqual 进行
 * 反解析；对它们只能反解析 indexqualorig 域，其中
 * 无 INDEX_VAR Var。）
 *
 * ancestors 列表是该 Plan 的父 Plan 与 SubPlan 节点的列表，
 * 嵌套最深者在前。这是解析 PARAM_EXEC 参数所需。
 * 注意我们假定所有 Plan 节点共享同一个范围表。
 *
 * 一旦调用过本函数，就可以对指定 Plan 节点的子表达式
 * 调用 deparse_expression()。要反解析同一 plan 树中另
 * 一个 Plan 节点的表达式，请重新调用本函数来指认新的
 * 父 Plan 节点。
 *
 * 结果就是传入的同一个 List；这只是记法上的便利。
 */
List *
set_deparse_context_plan(List *dpcontext, Plan *plan, List *ancestors)
{
	deparse_namespace *dpns;

	/* plan 反解析用：命名空间列表应总是只有一项 */
	Assert(list_length(dpcontext) == 1);
	dpns = (deparse_namespace *) linitial(dpcontext);

	/* 把注意力集中到传入的特定 plan 节点上 */
	dpns->ancestors = ancestors;
	set_deparse_plan(dpns, plan);

	return dpcontext;
}

/*
 * select_rtable_names_for_explain	- 为 EXPLAIN 选 RTE 别名
 *
 * 确定在 EXPLAIN 操作期间要使用的关系别名。这只是
 * set_rtable_names 的前端。我们必须把别名暴露给
 * EXPLAIN，因为 EXPLAIN 需要知道要打印的正确别名。
 */
List *
select_rtable_names_for_explain(List *rtable, Bitmapset *rels_used)
{
	deparse_namespace dpns;

	memset(&dpns, 0, sizeof(dpns));
	dpns.rtable = rtable;
	dpns.subplans = NIL;
	dpns.ctes = NIL;
	dpns.appendrels = NULL;
	set_rtable_names(&dpns, NIL, rels_used);
	/* 我们还无需费心计算列别名 */

	return dpns.rtable_names;
}

/*
 * set_rtable_names：选择用于打印查询的 RTE 别名
 *
 * 我们在 dpns->rtable_names 中填入一个与已填好的 dpns->rtable
 * 列表一一对应的别名列表。每个 RTE 名在新命名空间
 * 及 parent_namespaces 祖先命名空间中都是唯一
 * 的。
 *
 * 若 rels_used 非 NULL，只有其中列出的 RTE 索引才分配别名
 *
 * 注意本函数只关心关系名，而不关心列名。
 *
 */
static void
set_rtable_names(deparse_namespace *dpns, List *parent_namespaces,
				 Bitmapset *rels_used)
{
	HASHCTL		hash_ctl;
	HTAB	   *names_hash;
	NameHashEntry *hentry;
	bool		found;
	int			rtindex;
	ListCell   *lc;

	dpns->rtable_names = NIL;
	/* 空 rtable 则无需再做任何事*/
	if (dpns->rtable == NIL)
		return;

	/*
	 * 我们用哈希表保存已知名字，使本过程对 N 个名字是
	 * O(N) 而非 O(N^2)。
	 */
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(NameHashEntry);
	hash_ctl.hcxt = CurrentMemoryContext;
	names_hash = hash_create("set_rtable_names names",
							 list_length(dpns->rtable),
							 &hash_ctl,
							 HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	/* 以 parent_namespaces 中出现的名字预填充哈希表 */
	foreach(lc, parent_namespaces)
	{
		deparse_namespace *olddpns = (deparse_namespace *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, olddpns->rtable_names)
		{
			char	   *oldname = (char *) lfirst(lc2);

			if (oldname == NULL)
				continue;
			hentry = (NameHashEntry *) hash_search(names_hash,
												   oldname,
												   HASH_ENTER,
												   &found);
			/* 对父命名空间中的重复名字不作报错 */
			hentry->counter = 0;
		}
	}

	/* 现在扫描 rtable */
	rtindex = 1;
	foreach(lc, dpns->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		char	   *refname;

		/* 以防万一这里耗时超出合理范围…… */
		CHECK_FOR_INTERRUPTS();

		if (rels_used && !bms_is_member(rtindex, rels_used))
		{
			/* 忽略未引用的 RTE */
			refname = NULL;
		}
		else if (rte->alias)
		{
			/* 若 RTE 有自定义别名，则优先用之 */
			refname = rte->alias->aliasname;
		}
		else if (rte->rtekind == RTE_RELATION)
		{
			/* 使用该关系当前的实际名称 */
			refname = get_rel_name(rte->relid);
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/*未命名连接没有 refname*/
			refname = NULL;
		}
		else
		{
			/* 否则使用解析器分配的名字 */
			refname = rte->eref->aliasname;
		}

		/*
		 * 若选定的名字不唯一，则追加数字使其唯一，并且
		 * 一旦得到唯一名就为它建新哈希条目。对于
		 * 名字过长时，可能须截断以留在 NAMEDATALEN 内。
		 *
		 */
		if (refname)
		{
			hentry = (NameHashEntry *) hash_search(names_hash,
												   refname,
												   HASH_ENTER,
												   &found);
			if (found)
			{
				/* 名字已被占用，必须另选一个 */
				int			refnamelen = strlen(refname);
				char	   *modname = (char *) palloc(refnamelen + 16);
				NameHashEntry *hentry2;

				do
				{
					hentry->counter++;
					for (;;)
					{
						memcpy(modname, refname, refnamelen);
						sprintf(modname + refnamelen, "_%d", hentry->counter);
						if (strlen(modname) < NAMEDATALEN)
							break;
						/* 从 refname 删字符以保留全部数字 */
						refnamelen = pg_mbcliplen(refname, refnamelen,
												  refnamelen - 1);
					}
					hentry2 = (NameHashEntry *) hash_search(names_hash,
															modname,
															HASH_ENTER,
															&found);
				} while (found);
				hentry2->counter = 0;	/*初始化新哈希项*/
				refname = modname;
			}
			else
			{
				/* 名字此前未用过，只需初始化 hentry */
				hentry->counter = 0;
			}
		}

		dpns->rtable_names = lappend(dpns->rtable_names, refname);
		rtindex++;
	}

	hash_destroy(names_hash);
}

/*
 * set_deparse_for_query：为反解析 Query 树建立 deparse_namespace
 *
 * 为方便起见，本函数被定义为从头初始化 deparse_namespace
 * 结构。
 */
static void
set_deparse_for_query(deparse_namespace *dpns, Query *query,
					  List *parent_namespaces)
{
	ListCell   *lc;
	ListCell   *lc2;

	/* 初始化 *dpns 并填充 rtable/ctes 链接*/
	memset(dpns, 0, sizeof(deparse_namespace));
	dpns->rtable = query->rtable;
	dpns->subplans = NIL;
	dpns->ctes = query->cteList;
	dpns->appendrels = NULL;

	/* 给每个 RTE 分配唯一的关系别名 */
	set_rtable_names(dpns, parent_namespaces, NULL);

	/* 初始化 dpns->rtable_columns 为全零结构 */
	dpns->rtable_columns = NIL;
	while (list_length(dpns->rtable_columns) < list_length(dpns->rtable))
		dpns->rtable_columns = lappend(dpns->rtable_columns,
									   palloc0(sizeof(deparse_columns)));

	/* 若是 utility 查询，则不会有连接树 */
	if (query->jointree)
	{
		/* 检测是否需要对 USING 名做全局唯一化 */
		dpns->unique_using =
			has_dangerous_join_using(dpns, (Node *) query->jointree);

		/*
		 * 通过递归遍历查询连接树，为被 USING 合并的
		 * 列选择名字
		 */
		set_using_names(dpns, (Node *) query->jointree, NIL);
	}

	/*
	 * 现在为每个 RTE 分配其余的列别名。我们以对
	 * rtable 做线性扫描的方式进行，这样无论 RTE 是否在
	 * 连接树中都会被处理（我们不能漏掉 NEW.*、INSERT 目
	 * 标关系等）。JOIN RTE 必须在其子节点之后处理，但
	 * 这样可行，因为它们在 rtable 列表中位于子节点后
	 * （cf Asserts in identify_join_columns()）。
	 */
	forboth(lc, dpns->rtable, lc2, dpns->rtable_columns)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		deparse_columns *colinfo = (deparse_columns *) lfirst(lc2);

		if (rte->rtekind == RTE_JOIN)
			set_join_column_names(dpns, rte, colinfo);
		else
			set_relation_column_names(dpns, rte, colinfo);
	}
}

/*
 * set_simple_column_names：为非查询情形填入列别名
 *
 * 本函数处理 EXPLAIN 以及我们只有关系 RTE 的情形。
 * 没有连接树时，我们无法对连接 RTE 做任何巧妙处
 * 理，但也无需这么做（注意 EXPLAIN 反正永远不应见连接
 * 别名 Var）；若真遇到连接 RTE，就把它当非表基础 RTE 处理。
 */
static void
set_simple_column_names(deparse_namespace *dpns)
{
	ListCell   *lc;
	ListCell   *lc2;

	/* 初始化 dpns->rtable_columns 为全零结构 */
	dpns->rtable_columns = NIL;
	while (list_length(dpns->rtable_columns) < list_length(dpns->rtable))
		dpns->rtable_columns = lappend(dpns->rtable_columns,
									   palloc0(sizeof(deparse_columns)));

	/* 在每个 RTE 内分配唯一的列别名 */
	forboth(lc, dpns->rtable, lc2, dpns->rtable_columns)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		deparse_columns *colinfo = (deparse_columns *) lfirst(lc2);

		set_relation_column_names(dpns, rte, colinfo);
	}
}

/*
 * has_dangerous_join_using：在连接树中搜索未命名的
 * JOIN USING 列。
 * JOIN USING 合并列的行为可能不同于任一输入列，
 * 原因或是它们用 COALESCE 合并（FULL JOIN 时），或
 * 是因为需要对底层输入列做隐式类型转换。
 * 这种情况下该列必须作为 JOIN 的列来引用，而不是
 * 作为任一输入的列。若连接未命名（无别名），这就
 * 成了问题：我们无法用 RTE 名来限定该列的名字，
 * 因为根本没有名字。（强行给连接分配别名并非解决
 * 之法，那会阻止对连接下方表的合法引用。）
 * 为确保查询中每一列都能无歧义地引用，
 * 我们必须给这类合并列取全查询范围内全局唯一的
 * 名字，必要时给其他列改别名让路。
 *
 * 由于随之而来的重定别名相当有损查询可读性，
 * 除非迫不得已我们不做这件事。因此必须预扫描
 * 连接树判断是否必要，然后再启动 set_using_names()。
 */
static bool
has_dangerous_join_using(deparse_namespace *dpns, Node *jtnode)
{
	if (IsA(jtnode, RangeTblRef))
	{
		/* 此处无事可做 */
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (has_dangerous_join_using(dpns, (Node *) lfirst(lc)))
				return true;
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* 是带 USING 的未命名 JOIN？ */
		if (j->alias == NULL && j->usingClause)
		{
			/*
			 * 是，须检查每个连接别名 Var，看它们是否都
			 * 是底层列的简单引用。若是，就遇到了
			 * 危险局面，必须挑选唯一别名。
			 */
			RangeTblEntry *jrte = rt_fetch(j->rtindex, dpns->rtable);

			/* 我们只需检查合并列 */
			for (int i = 0; i < jrte->joinmergedcols; i++)
			{
				Node	   *aliasvar = list_nth(jrte->joinaliasvars, i);

				if (!IsA(aliasvar, Var))
					return true;
			}
		}

		/* 不是，但要查子节点*/
		if (has_dangerous_join_using(dpns, j->larg))
			return true;
		if (has_dangerous_join_using(dpns, j->rarg))
			return true;
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
	return false;
}

/*
 * set_using_names：为 USING 合并列选择要用的列别名
 *
 * 我们在查询连接树的递归下降中做此事。
 * dpns->unique_using 必须已设置好，以决定全局策略。
 *
 * 列别名信息保存在 dpns->rtable_columns 列表中，该
 * 列表假定已预填清零的 deparse_columns 结构。
 *
 * parentUsing 是在当前连接树节点的父连接中分配的
 * 全部 USING 别名的列表。（传入的列表不得修改。）
 */
static void
set_using_names(deparse_namespace *dpns, Node *jtnode, List *parentUsing)
{
	if (IsA(jtnode, RangeTblRef))
	{
		/* 现在无事可做*/
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
			set_using_names(dpns, (Node *) lfirst(lc), parentUsing);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		RangeTblEntry *rte = rt_fetch(j->rtindex, dpns->rtable);
		deparse_columns *colinfo = deparse_columns_fetch(j->rtindex, dpns);
		int		   *leftattnos;
		int		   *rightattnos;
		deparse_columns *leftcolinfo;
		deparse_columns *rightcolinfo;
		int			i;
		ListCell   *lc;

		/* 取得连接形态的相关信息 */
		identify_join_columns(j, rte, colinfo);
		leftattnos = colinfo->leftattnos;
		rightattnos = colinfo->rightattnos;

		/* 查找尚未填好的子 deparse_columns 结构 */
		leftcolinfo = deparse_columns_fetch(colinfo->leftrti, dpns);
		rightcolinfo = deparse_columns_fetch(colinfo->rightrti, dpns);

		/*
		 * 若此连接未命名，则本层无法替换新别名，因此
		 * 下推至此的名字要求必须再次下推到子节点。
		 *
		 */
		if (rte->alias == NULL)
		{
			for (i = 0; i < colinfo->num_cols; i++)
			{
				char	   *colname = colinfo->colnames[i];

				if (colname == NULL)
					continue;

				/* 下推到左列，除非它是系统列 */
				if (leftattnos[i] > 0)
				{
					expand_colnames_array_to(leftcolinfo, leftattnos[i]);
					leftcolinfo->colnames[leftattnos[i] - 1] = colname;
				}

				/* 右侧同样处理 */
				if (rightattnos[i] > 0)
				{
					expand_colnames_array_to(rightcolinfo, rightattnos[i]);
					rightcolinfo->colnames[rightattnos[i] - 1] = colname;
				}
			}
		}

		/*
		 * 若有 USING 子句，则选出 USING 列名并将其下推
		 * 到子节点。我们有两种策略：
		 *
		 * 若 dpns->unique_using 为真，我们强制所有 USING 名
		 * 在整个查询层内唯一。原则上我们只需要危险的
		 * USING 列名全局唯一，但为了在单次遍历中安全地
		 * 分配全部 USING 名，就必须对它们施行同一唯一性
		 * 规则。不过，若某 USING 列的名字已从父层下推
		 * 而来，我们就应按原样使用它，而不是做唯一性
		 * 调整。这在未命名连接处必要，且不会带来
		 * 歧义风险。另外，若某合并列有用户写的输出别
		 * 名，我们更愿意用它而非输入名；
		 *
		 * 这样做可简化逻辑，而且似乎整体上能减少改名。
		 *
		 *
		 * 若 dpns->unique_using 为假，我们只需要 USING 名
		 * 在其连接 RTE 内唯一。但仍需遵从下推名
		 * 字。
		 *
		 * 这两种策略结果大不相同，却由同一段代码
		 * 实现，唯一区别在于是否把分配的名字放入
		 * dpns->using_names 中。
		 */
		if (j->usingClause)
		{
			/* 复制输入的 parentUsing 列表以免修改它 */
			parentUsing = list_copy(parentUsing);

			/* USING 名必须对应最前面的连接输出列 */
			expand_colnames_array_to(colinfo, list_length(j->usingClause));
			i = 0;
			foreach(lc, j->usingClause)
			{
				char	   *colname = strVal(lfirst(lc));

				/* 断言这是合并列 */
				Assert(leftattnos[i] != 0 && rightattnos[i] != 0);

				/* 若有下推名则采用，否则选唯一名 */
				if (colinfo->colnames[i] != NULL)
					colname = colinfo->colnames[i];
				else
				{
					/* 若有用户写的输出别名则优先 */
					if (rte->alias && i < list_length(rte->alias->colnames))
						colname = strVal(list_nth(rte->alias->colnames, i));
					/* 使其适当地唯一 */
					colname = make_colname_unique(colname, dpns, colinfo);
					if (dpns->unique_using)
						dpns->using_names = lappend(dpns->using_names,
													colname);
					/* 同时把它存为输出列名 */
					colinfo->colnames[i] = colname;
				}

				/* 记住选定的名字供以后使用 */
				colinfo->usingNames = lappend(colinfo->usingNames, colname);
				parentUsing = lappend(parentUsing, colname);

				/* 下推到左列，除非它是系统列 */
				if (leftattnos[i] > 0)
				{
					expand_colnames_array_to(leftcolinfo, leftattnos[i]);
					leftcolinfo->colnames[leftattnos[i] - 1] = colname;
				}

				/* 右侧同样处理 */
				if (rightattnos[i] > 0)
				{
					expand_colnames_array_to(rightcolinfo, rightattnos[i]);
					rightcolinfo->colnames[rightattnos[i] - 1] = colname;
				}

				i++;
			}
		}

		/* 给子 deparse_columns 结构标记正确的 parentUsing 信息 */
		leftcolinfo->parentUsing = parentUsing;
		rightcolinfo->parentUsing = parentUsing;

		/* 现在递归地在子节点中分配 USING 列名 */
		set_using_names(dpns, j->larg, parentUsing);
		set_using_names(dpns, j->rarg, parentUsing);
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * set_relation_column_names：为非连接 RTE 选择列别名
 *
 * 列别名信息保存在 *colinfo 中，假定其已被预清零。
 * 若 colnames 中存在已填项，则它们优先于本地选择。
 *
 */
static void
set_relation_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
						  deparse_columns *colinfo)
{
	int			ncolumns;
	char	  **real_colnames;
	bool		changed_any;
	int			noldcolumns;
	int			i;
	int			j;

	/*
	 * 构造一个数组存放 RTE 当前的 "real" 列名。
	 * real_colnames[] 以物理列号为下标，已删除列的
	 * 项为 NULL。
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		/* 关系 --- 到系统目录中查找最新信息 */
		Relation	rel;
		TupleDesc	tupdesc;

		rel = relation_open(rte->relid, AccessShareLock);
		tupdesc = RelationGetDescr(rel);

		ncolumns = tupdesc->natts;
		real_colnames = (char **) palloc(ncolumns * sizeof(char *));

		for (i = 0; i < ncolumns; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				real_colnames[i] = NULL;
			else
				real_colnames[i] = pstrdup(NameStr(attr->attname));
		}
		relation_close(rel, AccessShareLock);
	}
	else
	{
		/* 否则从 eref 或 expandRTE() 取得列名 */
		List	   *colnames;
		ListCell   *lc;

		/*
		 * 返回复合类型的函数有个恼人性质：自查询被
		 * 解析以来，复合类型的某些列可能已被删除。如
		 * 可能则用 expandRTE() 来处理这种情况，因为它
		 * 具备查明已删列的繁琐逻辑。但若我们正在
		 * 解释 plan，那么我们没有 rte->functions，因为
		 * 规划器认为后面用不上它，这会破坏 expandRTE()。
		 * 所以这种情况下只能依赖 rte->eref，这可能导致
		 * 我们会报告已删列的旧名；对 EXPLAIN 而言
		 * 这似乎已经足够接近了。
		 *
		 * 对于非 RELATION、非 FUNCTION 的 RTE，直接看 rte->eref
		 * 即可，它应足够新：其他 RTE 类型解析后不会
		 * 有列被删掉。
		 */
		if (rte->rtekind == RTE_FUNCTION && rte->functions != NIL)
		{
			/* 因为我们不创建 Var，rtindex 等无关紧要 */
			expandRTE(rte, 1, 0, -1, true /* 含已删除列 */ ,
					  &colnames, NULL);
		}
		else
			colnames = rte->eref->colnames;

		ncolumns = list_length(colnames);
		real_colnames = (char **) palloc(ncolumns * sizeof(char *));

		i = 0;
		foreach(lc, colnames)
		{
			/*
			 * 若在此处找到的列名是空串，那它就是已删
			 * 除列，因此改为 NULL。
			 */
			char	   *cname = strVal(lfirst(lc));

			if (cname[0] == '\0')
				cname = NULL;
			real_colnames[i] = cname;
			i++;
		}
	}

	/*
	 * 确保 colinfo->colnames 为每列都有一个槽位。（如果
	 * 我们已经为最后一列下推过名字，它可能已经足够
	 * 长了。）注意：现在有可能列数比查询被解析时更
	 * 多，即 colnames 可能比 rte->eref->colnames 长。我们
	 * 也必须给新列分配唯一别名，否则视图/规则重新载
	 * 入时可能出现未解决的冲突。
	 */
	expand_colnames_array_to(colinfo, ncolumns);
	Assert(colinfo->num_cols == ncolumns);

	/*
	 * 也要把 new_colnames 与 is_new_col 数组建得足够大。
	 *
	 * 注意：由于在循环结束前 colinfo->num_new_cols 一直
	 * 保持为零，colname_is_unique 便不会查该数组，反正
	 * 也只是重复劳动。
	 */
	colinfo->new_colnames = (char **) palloc(ncolumns * sizeof(char *));
	colinfo->is_new_col = (bool *) palloc(ncolumns * sizeof(bool));

	/*
	 * 扫描各列，为每列选一个唯一别名，并把它存入
	 * colinfo->colnames 与 colinfo->new_colnames。前者数组
	 * 中已删除列为 NULL，后者略去它们。并标记
	 * new_colnames 项是否为解析时新增；超出
	 * rte->eref->colnames 长度的项就是这种情况。
	 */
	noldcolumns = list_length(rte->eref->colnames);
	changed_any = false;
	j = 0;
	for (i = 0; i < ncolumns; i++)
	{
		char	   *real_colname = real_colnames[i];
		char	   *colname = colinfo->colnames[i];

		/* 跳过已删除列 */
		if (real_colname == NULL)
		{
			Assert(colname == NULL);	/* colnames[i] 已是 NULL */
			continue;
		}

		/* 若已分配别名，就用它 */
		if (colname == NULL)
		{
			/* 若用户写过别名，则优先于真实列名使用它 */
			if (rte->alias && i < list_length(rte->alias->colnames))
				colname = strVal(list_nth(rte->alias->colnames, i));
			else
				colname = real_colname;

			/* 唯一化并插入 colinfo */
			colname = make_colname_unique(colname, dpns, colinfo);

			colinfo->colnames[i] = colname;
		}

		/* 把非删除列的名字也放进 new_colnames[] */
		colinfo->new_colnames[j] = colname;
		/* 并标记是否为新列 */
		colinfo->is_new_col[j] = (i >= noldcolumns);
		j++;

		/* 记住是否有别名与 "real" 名不同 */
		if (!changed_any && strcmp(colname, real_colname) != 0)
			changed_any = true;
	}

	/*
	 * 为 new_colnames[] 数组设置正确长度。（注意：若增
	 * 加了列，colinfo->num_cols 会把它们计入，这并不完
	 * 全正确但无害，因为新列必然位于末尾，不会影响
	 * 既有列的 varattno。）
	 */
	colinfo->num_new_cols = j;

	/*
	 * 对关系 RTE，只有别名列名与底层 "real" 名有任何
	 * 不同才需要打印。对函数 RTE，总是输出完整的列
	 * 别名列表；这是为了防范默认列名可能的不稳定
	 * （例如改参数名所致）。对 tablefunc RTE，我们从不
	 * 打印别名，因为列名是子句本身的一部分。对其
	 * 他 RTE 类型，若我们改动过任何东西，或有用户写
	 * 的列别名就打印
	 * （因为后者属于底层 "reality"）。
	 */
	if (rte->rtekind == RTE_RELATION)
		colinfo->printaliases = changed_any;
	else if (rte->rtekind == RTE_FUNCTION)
		colinfo->printaliases = true;
	else if (rte->rtekind == RTE_TABLEFUNC)
		colinfo->printaliases = false;
	else if (rte->alias && rte->alias->colnames != NIL)
		colinfo->printaliases = true;
	else
		colinfo->printaliases = changed_any;
}

/*
 * set_join_column_names：为连接 RTE 选择列别名
 *
 * 列别名信息保存在 *colinfo 中，假定其已被预清零。
 * 若 colnames 中存在已填项，则它们优先于本地选择。
 * 另外，USING 列的名字已由
 * set_using_names() 选定。我们还进一步期望两个输入
 * RTE 已完成列别名选择
 */
static void
set_join_column_names(deparse_namespace *dpns, RangeTblEntry *rte,
					  deparse_columns *colinfo)
{
	deparse_columns *leftcolinfo;
	deparse_columns *rightcolinfo;
	bool		changed_any;
	int			noldcolumns;
	int			nnewcolumns;
	Bitmapset  *leftmerged = NULL;
	Bitmapset  *rightmerged = NULL;
	int			i;
	int			j;
	int			ic;
	int			jc;

	/* 查找之前已填好的子 deparse_columns 结构 */
	leftcolinfo = deparse_columns_fetch(colinfo->leftrti, dpns);
	rightcolinfo = deparse_columns_fetch(colinfo->rightrti, dpns);

	/*
	 * 确保 colinfo->colnames 为每列都有一个槽位。（如果
	 * 我们已经为最后一列下推过名字，它可能已经足够
	 * 长了。）注意：现在可能一个或两个输入的列数
	 * 比解析时更多，但我们会在下面处理。我们
	 * 只需要 colnames 中既有列的项。
	 */
	noldcolumns = list_length(rte->eref->colnames);
	expand_colnames_array_to(colinfo, noldcolumns);
	Assert(colinfo->num_cols == noldcolumns);

	/*
	 * 扫描连接输出列，为每列选一个别名，并存入
	 * colinfo->colnames。若有 USING 列，set_using_names()
	 * 已选好它们的名字，因此循环可从第一个非合并
	 * 列开始。
	 */
	changed_any = false;
	for (i = list_length(colinfo->usingNames); i < noldcolumns; i++)
	{
		char	   *colname = colinfo->colnames[i];
		char	   *real_colname;

		/* 连接列必须引用至少一个输入列 */
		Assert(colinfo->leftattnos[i] != 0 || colinfo->rightattnos[i] != 0);

		/* 取得子列名 */
		if (colinfo->leftattnos[i] > 0)
			real_colname = leftcolinfo->colnames[colinfo->leftattnos[i] - 1];
		else if (colinfo->rightattnos[i] > 0)
			real_colname = rightcolinfo->colnames[colinfo->rightattnos[i] - 1];
		else
		{
			/* 我们连接的是系统列 --- 使用 eref 名*/
			real_colname = strVal(list_nth(rte->eref->colnames, i));
		}

		/* 若子列已被删除，则无需分配连接列名 */
		if (real_colname == NULL)
		{
			colinfo->colnames[i] = NULL;
			continue;
		}

		/* 在未命名连接中，直接按原样报告子列名 */
		if (rte->alias == NULL)
		{
			colinfo->colnames[i] = real_colname;
			continue;
		}

		/* 若已分配别名，就用它 */
		if (colname == NULL)
		{
			/* 若用户写过别名，则优先于真实列名使用它 */
			if (rte->alias && i < list_length(rte->alias->colnames))
				colname = strVal(list_nth(rte->alias->colnames, i));
			else
				colname = real_colname;

			/* 唯一化并插入 colinfo */
			colname = make_colname_unique(colname, dpns, colinfo);

			colinfo->colnames[i] = colname;
		}

		/* 记住是否有别名与 "real" 名不同 */
		if (!changed_any && strcmp(colname, real_colname) != 0)
			changed_any = true;
	}

	/*
	 * 计算若此连接现在重新解析将拥有的列数，并为
	 * new_colnames 与 is_new_col 数组创建存储。
	 *
	 * 注意：colname_is_unique 在下面的循环中会查询
	 * new_colnames[]，所以其未填的项必须为零。
	 */
	nnewcolumns = leftcolinfo->num_new_cols + rightcolinfo->num_new_cols -
		list_length(colinfo->usingNames);
	colinfo->num_new_cols = nnewcolumns;
	colinfo->new_colnames = (char **) palloc0(nnewcolumns * sizeof(char *));
	colinfo->is_new_col = (bool *) palloc0(nnewcolumns * sizeof(bool));

	/*
	 * 生成 new_colnames 数组有点棘手，因为解析后新增
	 * 的列必须插入到正确的位置。这段代码必须与解析
	 * 器一致：解析器会把连接的列排序为先是合并列
	 * （按 USING 子句顺序），然后是来自左输入的非合
	 * 并列（按 attnum 顺序），然后是来自右输入的同样
	 * 列（同理）。若某输入本身是连接，则它的列将按
	 * 同一规则排序，这意味着新增列未必
	 * 位于末尾。我们可以通过查阅
	 * leftattnos 和 rightattnos 数组以及输入的 is_new_col
	 *
	 * 在这些循环中，i 是 leftattnos/rightattnos 的下标
	 * （即连接 varattno 减一），j 是 new_colnames/is_new_col
	 * 下标；ic/jc 对子 RTE 类同。
	 */

	/* 处理合并列；它们最先且不可能为新列 */
	i = j = 0;
	while (i < noldcolumns &&
		   colinfo->leftattnos[i] != 0 &&
		   colinfo->rightattnos[i] != 0)
	{
		/* 列名已确定且已知唯一 */
		colinfo->new_colnames[j] = colinfo->colnames[i];
		colinfo->is_new_col[j] = false;

		/* 构建合并列的子 attnum 位图集 */
		if (colinfo->leftattnos[i] > 0)
			leftmerged = bms_add_member(leftmerged, colinfo->leftattnos[i]);
		if (colinfo->rightattnos[i] > 0)
			rightmerged = bms_add_member(rightmerged, colinfo->rightattnos[i]);

		i++, j++;
	}

	/* 处理左子节点的非合并列 */
	ic = 0;
	for (jc = 0; jc < leftcolinfo->num_new_cols; jc++)
	{
		char	   *child_colname = leftcolinfo->new_colnames[jc];

		if (!leftcolinfo->is_new_col[jc])
		{
			/* 把 ic 前进到左子节点下一个未删除的旧列 */
			while (ic < leftcolinfo->num_cols &&
				   leftcolinfo->colnames[ic] == NULL)
				ic++;
			Assert(ic < leftcolinfo->num_cols);
			ic++;
			/* 若它是合并列，我们已处理过它 */
			if (bms_is_member(ic, leftmerged))
				continue;
			/* 否则把 i 前进到对应的既有连接列 */
			while (i < colinfo->num_cols &&
				   colinfo->colnames[i] == NULL)
				i++;
			Assert(i < colinfo->num_cols);
			Assert(ic == colinfo->leftattnos[i]);
			/* 使用该列已分配的名字 */
			colinfo->new_colnames[j] = colinfo->colnames[i];
			i++;
		}
		else
		{
			/*
			 * 将新的子列名唯一化并赋值，除非我们处于
			 * 未命名连接中，此时只需复制
			 */
			if (rte->alias != NULL)
			{
				colinfo->new_colnames[j] =
					make_colname_unique(child_colname, dpns, colinfo);
				if (!changed_any &&
					strcmp(colinfo->new_colnames[j], child_colname) != 0)
					changed_any = true;
			}
			else
				colinfo->new_colnames[j] = child_colname;
		}

		colinfo->is_new_col[j] = leftcolinfo->is_new_col[jc];
		j++;
	}

	/* 以完全相同的方式处理右子节点的非合并列 */
	ic = 0;
	for (jc = 0; jc < rightcolinfo->num_new_cols; jc++)
	{
		char	   *child_colname = rightcolinfo->new_colnames[jc];

		if (!rightcolinfo->is_new_col[jc])
		{
			/* 把 ic 前进到右子节点下一个未删除的旧列 */
			while (ic < rightcolinfo->num_cols &&
				   rightcolinfo->colnames[ic] == NULL)
				ic++;
			Assert(ic < rightcolinfo->num_cols);
			ic++;
			/* 若它是合并列，我们已处理过它 */
			if (bms_is_member(ic, rightmerged))
				continue;
			/* 否则把 i 前进到对应的既有连接列 */
			while (i < colinfo->num_cols &&
				   colinfo->colnames[i] == NULL)
				i++;
			Assert(i < colinfo->num_cols);
			Assert(ic == colinfo->rightattnos[i]);
			/* 使用该列已分配的名字 */
			colinfo->new_colnames[j] = colinfo->colnames[i];
			i++;
		}
		else
		{
			/*
			 * 将新的子列名唯一化并赋值，除非我们处于
			 * 未命名连接中，此时只需复制
			 */
			if (rte->alias != NULL)
			{
				colinfo->new_colnames[j] =
					make_colname_unique(child_colname, dpns, colinfo);
				if (!changed_any &&
					strcmp(colinfo->new_colnames[j], child_colname) != 0)
					changed_any = true;
			}
			else
				colinfo->new_colnames[j] = child_colname;
		}

		colinfo->is_new_col[j] = rightcolinfo->is_new_col[jc];
		j++;
	}

	/* 断言我们处理的列数正确 */
#ifdef USE_ASSERT_CHECKING
	while (i < colinfo->num_cols && colinfo->colnames[i] == NULL)
		i++;
	Assert(i == colinfo->num_cols);
	Assert(j == nnewcolumns);
#endif

	/*
	 * 对命名连接，若列名相对子列名有改动则打印别名。
	 * 未命名连接不能打印别名。
	 */
	if (rte->alias != NULL)
		colinfo->printaliases = changed_any;
	else
		colinfo->printaliases = false;
}

/*
 * colname_is_unique：colname 是否区别于已选定的列名？
 *
 * dpns 是查询级信息，colinfo 属于该列的 RTE
 */
static bool
colname_is_unique(const char *colname, deparse_namespace *dpns,
				  deparse_columns *colinfo)
{
	int			i;
	ListCell   *lc;

	/* 对照 RTE 内已分配的列别名检查 */
	for (i = 0; i < colinfo->num_cols; i++)
	{
		char	   *oldname = colinfo->colnames[i];

		if (oldname && strcmp(oldname, colname) == 0)
			return false;
	}

	/*
	 * 若我们在构建 new_colnames 数组，也要检查它（这
	 * 与前面的检查部分重复但不完全重复）
	 */
	for (i = 0; i < colinfo->num_new_cols; i++)
	{
		char	   *oldname = colinfo->new_colnames[i];

		if (oldname && strcmp(oldname, colname) == 0)
			return false;
	}

	/* 还要对照必须全局唯一的 USING 列名检查 */
	foreach(lc, dpns->using_names)
	{
		char	   *oldname = (char *) lfirst(lc);

		if (strcmp(oldname, colname) == 0)
			return false;
	}

	/* 还要对照已为父连接 USING 列分配的名字检查 */
	foreach(lc, colinfo->parentUsing)
	{
		char	   *oldname = (char *) lfirst(lc);

		if (strcmp(oldname, colname) == 0)
			return false;
	}

	return true;
}

/*
 * make_colname_unique：必要时修改 colname 使其唯一
 *
 * dpns 是查询级信息，colinfo 属于该列的 RTE
 */
static char *
make_colname_unique(char *colname, deparse_namespace *dpns,
					deparse_columns *colinfo)
{
	/*
	 * 若选定的名字不唯一，则追加数字使其唯一。对于
	 * 名字过长时，可能须截断以留在 NAMEDATALEN 内。
	 *
	 */
	if (!colname_is_unique(colname, dpns, colinfo))
	{
		int			colnamelen = strlen(colname);
		char	   *modname = (char *) palloc(colnamelen + 16);
		int			i = 0;

		do
		{
			i++;
			for (;;)
			{
				memcpy(modname, colname, colnamelen);
				sprintf(modname + colnamelen, "_%d", i);
				if (strlen(modname) < NAMEDATALEN)
					break;
				/* 从 colname 删字符以保留全部数字 */
				colnamelen = pg_mbcliplen(colname, colnamelen,
										  colnamelen - 1);
			}
		} while (!colname_is_unique(modname, dpns, colinfo));
		colname = modname;
	}
	return colname;
}

/*
 * expand_colnames_array_to：使 colinfo->colnames 至少有 n 项长
 *
 * 新增的数组项都初始化为零。
 */
static void
expand_colnames_array_to(deparse_columns *colinfo, int n)
{
	if (n > colinfo->num_cols)
	{
		if (colinfo->colnames == NULL)
			colinfo->colnames = palloc0_array(char *, n);
		else
			colinfo->colnames = repalloc0_array(colinfo->colnames, char *, colinfo->num_cols, n);
		colinfo->num_cols = n;
	}
}

/*
 * identify_join_columns：弄清连接的列来自何处
 *
 * 填充 colinfo 结构的连接特有字段，但 usingNames
 * 除外，它稍后填充。
 */
static void
identify_join_columns(JoinExpr *j, RangeTblEntry *jrte,
					  deparse_columns *colinfo)
{
	int			numjoincols;
	int			jcolno;
	int			rcolno;
	ListCell   *lc;

	/* 提取左/右子 RT 索引 */
	if (IsA(j->larg, RangeTblRef))
		colinfo->leftrti = ((RangeTblRef *) j->larg)->rtindex;
	else if (IsA(j->larg, JoinExpr))
		colinfo->leftrti = ((JoinExpr *) j->larg)->rtindex;
	else
		elog(ERROR, "unrecognized node type in jointree: %d",
			 (int) nodeTag(j->larg));
	if (IsA(j->rarg, RangeTblRef))
		colinfo->rightrti = ((RangeTblRef *) j->rarg)->rtindex;
	else if (IsA(j->rarg, JoinExpr))
		colinfo->rightrti = ((JoinExpr *) j->rarg)->rtindex;
	else
		elog(ERROR, "unrecognized node type in jointree: %d",
			 (int) nodeTag(j->rarg));

	/* 断言子节点在第二遍中会先于连接被处理 */
	Assert(colinfo->leftrti < j->rtindex);
	Assert(colinfo->rightrti < j->rtindex);

	/* 用零初始化结果数组 */
	numjoincols = list_length(jrte->joinaliasvars);
	Assert(numjoincols == list_length(jrte->eref->colnames));
	colinfo->leftattnos = (int *) palloc0(numjoincols * sizeof(int));
	colinfo->rightattnos = (int *) palloc0(numjoincols * sizeof(int));

	/*
	 * 把 RTE 的 joinleftcols/joinrightcols 分解成所需格
	 * 式。回顾一下：因 USING 而合并的列是连接输出的
	 * 第一列。扫描 joinleftcols 时无需做任何特殊处理，
	 * 但扫描 joinrightcols 时必须区分合并列与非合并列。
	 *
	 */
	jcolno = 0;
	foreach(lc, jrte->joinleftcols)
	{
		int			leftattno = lfirst_int(lc);

		colinfo->leftattnos[jcolno++] = leftattno;
	}
	rcolno = 0;
	foreach(lc, jrte->joinrightcols)
	{
		int			rightattno = lfirst_int(lc);

		if (rcolno < jrte->joinmergedcols)	/* 合并列？ */
			colinfo->rightattnos[rcolno] = rightattno;
		else
			colinfo->rightattnos[jcolno++] = rightattno;
		rcolno++;
	}
	Assert(jcolno == numjoincols);
}

/*
 * get_rtable_name：获取先前分配的 RTE 别名的便利函数
 *
 * 该 RTE 必须属于 "context" 最顶层的命名空间层。
 */
static char *
get_rtable_name(int rtindex, deparse_context *context)
{
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);

	Assert(rtindex > 0 && rtindex <= list_length(dpns->rtable_names));
	return (char *) list_nth(dpns->rtable_names, rtindex - 1);
}

/*
 * set_deparse_plan：建立 deparse_namespace 以解析给定
 * Plan 的子表达式
 *
 * 本函数设置 plan、outer_plan、inner_plan、outer_tlist、
 * inner_tlist 与 index_tlist 字段。调用方必须已按需要
 * 调整过 ancestors 列表。注意 rtable、subplans、ctes
 * 字段在转向同一 plan 树内不同 plan 节点时无需改变
 *
 */
static void
set_deparse_plan(deparse_namespace *dpns, Plan *plan)
{
	dpns->plan = plan;

	/*
	 * 我们对 Append 与 MergeAppend 特殊处理，假装第一个
	 * 子 plan 是 OUTER 引用对象；我们必须把其 tlist 中的
	 * OUTER Var 按某个子节点来解释，而第一个是最自然
	 * 的选择。
	 */
	if (IsA(plan, Append))
		dpns->outer_plan = linitial(((Append *) plan)->appendplans);
	else if (IsA(plan, MergeAppend))
		dpns->outer_plan = linitial(((MergeAppend *) plan)->mergeplans);
	else
		dpns->outer_plan = outerPlan(plan);

	if (dpns->outer_plan)
		dpns->outer_tlist = dpns->outer_plan->targetlist;
	else
		dpns->outer_tlist = NIL;

	/*
	 * 对 SubqueryScan，假装子 plan 是 INNER 引用对象。（
	 * 我们不用 OUTER，因为那将来可能与正常含义冲突。）
	 * 同样，对 CteScan，假装子查询的 plan 是 INNER 引用
	 * 对象。对 WorkTableScan，则定位为 INNER 引用对象的
	 * 父 RecursiveUnion。
	 *
	 * 对 MERGE，假装 ModifyTable 的源 plan（其 outer plan）
	 * 是 INNER 引用对象。这就是从目标关系到数据源的
	 * 连接，查询其他部分的所有 INNER_VAR Var 都引用它
	 * 的 tlist
	 *
	 * 对 ON CONFLICT .. UPDATE，我们只需让 inner tlist 指向
	 * excluded 表达式的 tlist。（与 SubqueryScan 类似，我们
	 * 不想复用 OUTER：它并非用于 INSERT .. CONFLICT，
	 * 而是用于 RETURNING）。
	 */
	if (IsA(plan, SubqueryScan))
		dpns->inner_plan = ((SubqueryScan *) plan)->subplan;
	else if (IsA(plan, CteScan))
		dpns->inner_plan = list_nth(dpns->subplans,
									((CteScan *) plan)->ctePlanId - 1);
	else if (IsA(plan, WorkTableScan))
		dpns->inner_plan = find_recursive_union(dpns,
												(WorkTableScan *) plan);
	else if (IsA(plan, ModifyTable))
	{
		if (((ModifyTable *) plan)->operation == CMD_MERGE)
			dpns->inner_plan = outerPlan(plan);
		else
			dpns->inner_plan = plan;
	}
	else
		dpns->inner_plan = innerPlan(plan);

	if (IsA(plan, ModifyTable) && ((ModifyTable *) plan)->operation == CMD_INSERT)
		dpns->inner_tlist = ((ModifyTable *) plan)->exclRelTlist;
	else if (dpns->inner_plan)
		dpns->inner_tlist = dpns->inner_plan->targetlist;
	else
		dpns->inner_tlist = NIL;

	/* 如需，为 INDEX_VAR Var 设置引用对象 */
	if (IsA(plan, IndexOnlyScan))
		dpns->index_tlist = ((IndexOnlyScan *) plan)->indextlist;
	else if (IsA(plan, ForeignScan))
		dpns->index_tlist = ((ForeignScan *) plan)->fdw_scan_tlist;
	else if (IsA(plan, CustomScan))
		dpns->index_tlist = ((CustomScan *) plan)->custom_scan_tlist;
	else
		dpns->index_tlist = NIL;
}

/*
 * 找到作为生成 WorkTableScan 工作表的 RecursiveUnion
 * 的祖先 plan 节点。我们可以按 wtParam 匹配，因为它
 * 在 plan 树内应当唯一。
 */
static Plan *
find_recursive_union(deparse_namespace *dpns, WorkTableScan *wtscan)
{
	ListCell   *lc;

	foreach(lc, dpns->ancestors)
	{
		Plan	   *ancestor = (Plan *) lfirst(lc);

		if (IsA(ancestor, RecursiveUnion) &&
			((RecursiveUnion *) ancestor)->wtParam == wtscan->wtParam)
			return ancestor;
	}
	elog(ERROR, "could not find RecursiveUnion for WorkTableScan with wtParam %d",
		 wtscan->wtParam);
	return NULL;
}

/*
 * push_child_plan：临时把反解析注意力转移到子 plan
 *
 * 在展开 OUTER_VAR 或 INNER_VAR 引用时，我们必须调整
 * 反解析上下文，以防被引用的表达式本身用到
 * OUTER_VAR/INNER_VAR。我们就地修改栈顶项，以避免
 * 影响 levelsup 问题（尽管在 Plan 树中本来就不应该
 * 有任何此类情况）。
 *
 * 调用方必须提供一个局部 deparse_namespace 变量来保
 * 存 pop_child_plan 先前状态。
 */
static void
push_child_plan(deparse_namespace *dpns, Plan *plan,
				deparse_namespace *save_dpns)
{
	/* 保存状态供以后恢复 */
	*save_dpns = *dpns;

	/*把当前 plan 节点链入 ancestors 列表*/
	dpns->ancestors = lcons(dpns->plan, dpns->ancestors);

	/* 注意力转向所选子节点 */
	set_deparse_plan(dpns, plan);
}

/*
 * pop_child_plan：撤销 push_child_plan 的效果
 */
static void
pop_child_plan(deparse_namespace *dpns, deparse_namespace *save_dpns)
{
	List	   *ancestors;

	/* 去掉 push_child_plan 加入的 ancestors 列表单元 */
	ancestors = list_delete_first(dpns->ancestors);

	/* 恢复被 push_child_plan 改变的字段 */
	*dpns = *save_dpns;

	/* 确保 dpns->ancestors 正确（可能不必要） */
	dpns->ancestors = ancestors;
}

/*
 * push_ancestor_plan：临时把反解析注意力转移到
 * 祖先 plan
 *
 * 在展开 Param 引用时，我们必须调整反解析上下文
 * 使之与包含待打印表达式的 plan 节点相匹配；
 * 否则，若该表达式本身包含 Param 或 OUTER_VAR/
 * INNER_VAR/INDEX_VAR，就会失败。
 *
 * 目标祖先可用 dpns->ancestors 中持有它的那个
 * ListCell 标识。
 *
 * 调用方必须提供一个局部 deparse_namespace 变量来保
 * 存 pop_ancestor_plan 先前状态。
 */
static void
push_ancestor_plan(deparse_namespace *dpns, ListCell *ancestor_cell,
				   deparse_namespace *save_dpns)
{
	Plan	   *plan = (Plan *) lfirst(ancestor_cell);

	/* 保存状态供以后恢复 */
	*save_dpns = *dpns;

	/* 构建一个只含本节点祖先的新 ancestors 列表 */
	dpns->ancestors =
		list_copy_tail(dpns->ancestors,
					   list_cell_number(dpns->ancestors, ancestor_cell) + 1);

	/* 把注意力集中到所选祖先 */
	set_deparse_plan(dpns, plan);
}

/*
 * pop_ancestor_plan：撤销 push_ancestor_plan 的效果
 */
static void
pop_ancestor_plan(deparse_namespace *dpns, deparse_namespace *save_dpns)
{
	/* 释放 push_ancestor_plan 中建立的祖先列表*/
	list_free(dpns->ancestors);

	/* 恢复被 push_ancestor_plan 改变的字段 */
	*dpns = *save_dpns;
}


/* ----------
 * make_ruledef			- 重建 CREATE RULE 命令
 *				  针对 pg_rewrite 元组
 * ----------
 */
static void
make_ruledef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags)
{
	char	   *rulename;
	char		ev_type;
	Oid			ev_class;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions;
	Relation	ev_relation;
	TupleDesc	viewResultDesc = NULL;
	int			fno;
	Datum		dat;
	bool		isnull;

	/*
	 * 从规则元组取得属性值
	 */
	fno = SPI_fnumber(rulettc, "rulename");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	rulename = NameStr(*(DatumGetName(dat)));

	fno = SPI_fnumber(rulettc, "ev_type");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_type = DatumGetChar(dat);

	fno = SPI_fnumber(rulettc, "ev_class");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_class = DatumGetObjectId(dat);

	fno = SPI_fnumber(rulettc, "is_instead");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	is_instead = DatumGetBool(dat);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_qual != NULL);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_action != NULL);
	actions = (List *) stringToNode(ev_action);
	if (actions == NIL)
		elog(ERROR, "invalid empty ev_action list");

	ev_relation = table_open(ev_class, AccessShareLock);

	/*
	 * 构建规则定义文本
	 */
	appendStringInfo(buf, "CREATE RULE %s AS",
					 quote_identifier(rulename));

	if (prettyFlags & PRETTYFLAG_INDENT)
		appendStringInfoString(buf, "\n    ON ");
	else
		appendStringInfoString(buf, " ON ");

	/* 规则所针对的事件 */
	switch (ev_type)
	{
		case '1':
			appendStringInfoString(buf, "SELECT");
			viewResultDesc = RelationGetDescr(ev_relation);
			break;

		case '2':
			appendStringInfoString(buf, "UPDATE");
			break;

		case '3':
			appendStringInfoString(buf, "INSERT");
			break;

		case '4':
			appendStringInfoString(buf, "DELETE");
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("rule \"%s\" has unsupported event type %d",
							rulename, ev_type)));
			break;
	}

	/* 规则所作用的关系 */
	appendStringInfo(buf, " TO %s",
					 (prettyFlags & PRETTYFLAG_SCHEMA) ?
					 generate_relation_name(ev_class, NIL) :
					 generate_qualified_relation_name(ev_class));

	/* 若规则有事件条件，则把它加上 */
	if (strcmp(ev_qual, "<>") != 0)
	{
		Node	   *qual;
		Query	   *query;
		deparse_context context;
		deparse_namespace dpns;

		if (prettyFlags & PRETTYFLAG_INDENT)
			appendStringInfoString(buf, "\n  ");
		appendStringInfoString(buf, " WHERE ");

		qual = stringToNode(ev_qual);

		/*
		 * 我们需要构造上下文来识别条件里的任何 Var
		 * （它们只能是对 OLD 和 NEW 的引用）。为此使用
		 * 动作列表中第一个查询的 rtable。
		 */
		query = (Query *) linitial(actions);

		/*
		 * 若动作是 INSERT...SELECT，则 OLD/NEW 已被下推进
		 * SELECT，我们要看的正是它。（丑陋的权宜之计
		 * 待重新设计查询树时再修好。）
		 */
		query = getInsertSelectQuery(query, NULL);

		/* 必须立刻获取锁；参见 get_query_def() 中的说明 */
		AcquireRewriteLocks(query, false, false);

		context.buf = buf;
		context.namespaces = list_make1(&dpns);
		context.resultDesc = NULL;
		context.targetList = NIL;
		context.windowClause = NIL;
		context.varprefix = (list_length(query->rtable) != 1);
		context.prettyFlags = prettyFlags;
		context.wrapColumn = WRAP_COLUMN_DEFAULT;
		context.indentLevel = PRETTYINDENT_STD;
		context.colNamesVisible = true;
		context.inGroupBy = false;
		context.varInOrderBy = false;
		context.appendparents = NULL;

		set_deparse_for_query(&dpns, query, NIL);

		get_rule_expr(qual, &context, false);
	}

	appendStringInfoString(buf, " DO ");

	/*INSTEAD 关键字（如是）*/
	if (is_instead)
		appendStringInfoString(buf, "INSTEAD ");

	/* 最后是规则动作 */
	if (list_length(actions) > 1)
	{
		ListCell   *action;
		Query	   *query;

		appendStringInfoChar(buf, '(');
		foreach(action, actions)
		{
			query = (Query *) lfirst(action);
			get_query_def(query, buf, NIL, viewResultDesc, true,
						  prettyFlags, WRAP_COLUMN_DEFAULT, 0);
			if (prettyFlags)
				appendStringInfoString(buf, ";\n");
			else
				appendStringInfoString(buf, "; ");
		}
		appendStringInfoString(buf, ");");
	}
	else
	{
		Query	   *query;

		query = (Query *) linitial(actions);
		get_query_def(query, buf, NIL, viewResultDesc, true,
					  prettyFlags, WRAP_COLUMN_DEFAULT, 0);
		appendStringInfoChar(buf, ';');
	}

	table_close(ev_relation, AccessShareLock);
}


/* ----------
 * make_viewdef			- 重建视图重写规则的
 *				  SELECT 部分
 * ----------
 */
static void
make_viewdef(StringInfo buf, HeapTuple ruletup, TupleDesc rulettc,
			 int prettyFlags, int wrapColumn)
{
	Query	   *query;
	char		ev_type;
	Oid			ev_class;
	bool		is_instead;
	char	   *ev_qual;
	char	   *ev_action;
	List	   *actions;
	Relation	ev_relation;
	int			fno;
	Datum		dat;
	bool		isnull;

	/*
	 * 从规则元组取得属性值
	 */
	fno = SPI_fnumber(rulettc, "ev_type");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_type = DatumGetChar(dat);

	fno = SPI_fnumber(rulettc, "ev_class");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	ev_class = DatumGetObjectId(dat);

	fno = SPI_fnumber(rulettc, "is_instead");
	dat = SPI_getbinval(ruletup, rulettc, fno, &isnull);
	Assert(!isnull);
	is_instead = DatumGetBool(dat);

	fno = SPI_fnumber(rulettc, "ev_qual");
	ev_qual = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_qual != NULL);

	fno = SPI_fnumber(rulettc, "ev_action");
	ev_action = SPI_getvalue(ruletup, rulettc, fno);
	Assert(ev_action != NULL);
	actions = (List *) stringToNode(ev_action);

	if (list_length(actions) != 1)
	{
		/*保持输出缓冲区为空并返回*/
		return;
	}

	query = (Query *) linitial(actions);

	if (ev_type != '1' || !is_instead ||
		strcmp(ev_qual, "<>") != 0 || query->commandType != CMD_SELECT)
	{
		/*保持输出缓冲区为空并返回*/
		return;
	}

	ev_relation = table_open(ev_class, AccessShareLock);

	get_query_def(query, buf, NIL, RelationGetDescr(ev_relation), true,
				  prettyFlags, wrapColumn, 0);
	appendStringInfoChar(buf, ';');

	table_close(ev_relation, AccessShareLock);
}


/* ----------
 * get_query_def			- 反解析查询解析树
 *
 * query：待显示的解析树
 * buf：输出文本追加到 buf
 * parentnamespace：外层 deparse_namespace 的列表（初始时为空）
 * resultDesc：若非 NULL，则视图（由 SELECT 查询表示）
 *		的输出元组描述符。我们使用其中的列名来
 *		标记 SELECT 输出列，优先于查询中的名字
 * colNamesVisible：若外围上下文关心输出列名则为
 *		真（例如 EXISTS() 上下文就不关心）；为假时我们
 *		可省略 "?column?" 这类哑列标签。
 * prettyFlags：PRETTYFLAG_XXX 选项位掩码
 * wrapColumn：最大行宽，-1 表示禁止换行
 * startIndent：初始缩进量
 * ----------
 */
static void
get_query_def(Query *query, StringInfo buf, List *parentnamespace,
			  TupleDesc resultDesc, bool colNamesVisible,
			  int prettyFlags, int wrapColumn, int startIndent)
{
	deparse_context context;
	deparse_namespace dpns;

	/* 防范过长或嵌套过深的查询 */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	/*
	 * 在检查查询之前，先给被引用关系加锁，并修复
	 * JOIN RTE 中已删除的列。这保证结果一致。注意我
	 * 们假定可以直接涂改传入的查询树！
	 *
	 *
	 * 我们只是反解析查询（不会执行它），所以只需
	 * 要它所提到关系上的 AccessShareLock。
	 */
	AcquireRewriteLocks(query, false, false);

	context.buf = buf;
	context.namespaces = lcons(&dpns, list_copy(parentnamespace));
	context.resultDesc = NULL;
	context.targetList = NIL;
	context.windowClause = NIL;
	context.varprefix = (parentnamespace != NIL ||
						 list_length(query->rtable) != 1);
	context.prettyFlags = prettyFlags;
	context.wrapColumn = wrapColumn;
	context.indentLevel = startIndent;
	context.colNamesVisible = colNamesVisible;
	context.inGroupBy = false;
	context.varInOrderBy = false;
	context.appendparents = NULL;

	set_deparse_for_query(&dpns, query, parentnamespace);

	switch (query->commandType)
	{
		case CMD_SELECT:
			/* 仅当是 SELECT 时才设置 context.resultDesc*/
			context.resultDesc = resultDesc;
			get_select_query_def(query, &context);
			break;

		case CMD_UPDATE:
			get_update_query_def(query, &context);
			break;

		case CMD_INSERT:
			get_insert_query_def(query, &context);
			break;

		case CMD_DELETE:
			get_delete_query_def(query, &context);
			break;

		case CMD_MERGE:
			get_merge_query_def(query, &context);
			break;

		case CMD_NOTHING:
			appendStringInfoString(buf, "NOTHING");
			break;

		case CMD_UTILITY:
			get_utility_query_def(query, &context);
			break;

		default:
			elog(ERROR, "unrecognized query command type: %d",
				 query->commandType);
			break;
	}
}

/* ----------
 * get_values_def			- 反解析 VALUES 列表
 * ----------
 */
static void
get_values_def(List *values_lists, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first_list = true;
	ListCell   *vtl;

	appendStringInfoString(buf, "VALUES ");

	foreach(vtl, values_lists)
	{
		List	   *sublist = (List *) lfirst(vtl);
		bool		first_col = true;
		ListCell   *lc;

		if (first_list)
			first_list = false;
		else
			appendStringInfoString(buf, ", ");

		appendStringInfoChar(buf, '(');
		foreach(lc, sublist)
		{
			Node	   *col = (Node *) lfirst(lc);

			if (first_col)
				first_col = false;
			else
				appendStringInfoChar(buf, ',');

			/*
			 * 打印该值。整行 Var 需要特殊处理。
			 */
			get_rule_expr_toplevel(col, context, false);
		}
		appendStringInfoChar(buf, ')');
	}
}

/* ----------
 * get_with_clause			- 反解析 WITH 子句
 * ----------
 */
static void
get_with_clause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	if (query->cteList == NIL)
		return;

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}

	if (query->hasRecursive)
		sep = "WITH RECURSIVE ";
	else
		sep = "WITH ";
	foreach(l, query->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(l);

		appendStringInfoString(buf, sep);
		appendStringInfoString(buf, quote_identifier(cte->ctename));
		if (cte->aliascolnames)
		{
			bool		first = true;
			ListCell   *col;

			appendStringInfoChar(buf, '(');
			foreach(col, cte->aliascolnames)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(col))));
			}
			appendStringInfoChar(buf, ')');
		}
		appendStringInfoString(buf, " AS ");
		switch (cte->ctematerialized)
		{
			case CTEMaterializeDefault:
				break;
			case CTEMaterializeAlways:
				appendStringInfoString(buf, "MATERIALIZED ");
				break;
			case CTEMaterializeNever:
				appendStringInfoString(buf, "NOT MATERIALIZED ");
				break;
		}
		appendStringInfoChar(buf, '(');
		if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", 0, 0, 0);
		get_query_def((Query *) cte->ctequery, buf, context->namespaces, NULL,
					  true,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
		if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", 0, 0, 0);
		appendStringInfoChar(buf, ')');

		if (cte->search_clause)
		{
			bool		first = true;
			ListCell   *lc;

			appendStringInfo(buf, " SEARCH %s FIRST BY ",
							 cte->search_clause->search_breadth_first ? "BREADTH" : "DEPTH");

			foreach(lc, cte->search_clause->search_col_list)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(lc))));
			}

			appendStringInfo(buf, " SET %s", quote_identifier(cte->search_clause->search_seq_column));
		}

		if (cte->cycle_clause)
		{
			bool		first = true;
			ListCell   *lc;

			appendStringInfoString(buf, " CYCLE ");

			foreach(lc, cte->cycle_clause->cycle_col_list)
			{
				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf,
									   quote_identifier(strVal(lfirst(lc))));
			}

			appendStringInfo(buf, " SET %s", quote_identifier(cte->cycle_clause->cycle_mark_column));

			{
				Const	   *cmv = castNode(Const, cte->cycle_clause->cycle_mark_value);
				Const	   *cmd = castNode(Const, cte->cycle_clause->cycle_mark_default);

				if (!(cmv->consttype == BOOLOID && !cmv->constisnull && DatumGetBool(cmv->constvalue) == true &&
					  cmd->consttype == BOOLOID && !cmd->constisnull && DatumGetBool(cmd->constvalue) == false))
				{
					appendStringInfoString(buf, " TO ");
					get_rule_expr(cte->cycle_clause->cycle_mark_value, context, false);
					appendStringInfoString(buf, " DEFAULT ");
					get_rule_expr(cte->cycle_clause->cycle_mark_default, context, false);
				}
			}

			appendStringInfo(buf, " USING %s", quote_identifier(cte->cycle_clause->cycle_path_column));
		}

		sep = ", ";
	}

	if (PRETTY_INDENT(context))
	{
		context->indentLevel -= PRETTYINDENT_STD;
		appendContextKeyword(context, "", 0, 0, 0);
	}
	else
		appendStringInfoChar(buf, ' ');
}

/* ----------
 * get_select_query_def			- 反解析 SELECT 解析树
 * ----------
 */
static void
get_select_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		force_colno;
	ListCell   *l;

	/* 若有 WITH 子句则插入 */
	get_with_clause(query, context);

	/* 子例程可能需要查阅 SELECT 目标列表与 windowClause */
	context->targetList = query->targetList;
	context->windowClause = query->windowClause;

	/*
	 * 若 Query 节点有 setOperations 树，则它是
	 * UNION/INTERSECT/EXCEPT 查询的顶层；此时顶层查询
	 * 本身仅 WITH、ORDER BY、LIMIT 有意义。
	 */
	if (query->setOperations)
	{
		get_setop_query(query->setOperations, query, context);
		/* 此时 ORDER BY 子句必须简单 */
		force_colno = true;
	}
	else
	{
		get_basic_select_query(query, context);
		force_colno = false;
	}

	/* 若有 ORDER BY 子句则添加 */
	if (query->sortClause != NIL)
	{
		appendContextKeyword(context, " ORDER BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_orderby(query->sortClause, query->targetList,
						 force_colno, context);
	}

	/*
	 * 若有 LIMIT/OFFSET 子句则添加。若非默认选项，则使用
	 * LIMIT 的标准拼写。
	 */
	if (query->limitOffset != NULL)
	{
		appendContextKeyword(context, " OFFSET ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->limitOffset, context, false);
	}
	if (query->limitCount != NULL)
	{
		if (query->limitOption == LIMIT_OPTION_WITH_TIES)
		{
			/*
			 * limitCount 参数是 c_expr，所以需要括号。简单
			 * 字面量与函数表达式本不需要括号，但
			 * 遗憾的是，难断定表达式打印成形如 123 的
			 * 简单字面量，还是 '-123'::int4 型转换
			 * 表达式。文法接受前者不带引号的形式，但后
			 * 者不行。
			 */
			appendContextKeyword(context, " FETCH FIRST ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
			appendStringInfoChar(buf, '(');
			get_rule_expr(query->limitCount, context, false);
			appendStringInfoChar(buf, ')');
			appendStringInfoString(buf, " ROWS WITH TIES");
		}
		else
		{
			appendContextKeyword(context, " LIMIT ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
			if (IsA(query->limitCount, Const) &&
				((Const *) query->limitCount)->constisnull)
				appendStringInfoString(buf, "ALL");
			else
				get_rule_expr(query->limitCount, context, false);
		}
	}

	/* 若有 FOR [KEY] UPDATE/SHARE 子句则添加 */
	if (query->hasForUpdate)
	{
		foreach(l, query->rowMarks)
		{
			RowMarkClause *rc = (RowMarkClause *) lfirst(l);

			/* 不打印隐式子句 */
			if (rc->pushedDown)
				continue;

			switch (rc->strength)
			{
				case LCS_NONE:
					/* 对 LCS_NONE 我们有意抛出错误 */
					elog(ERROR, "unrecognized LockClauseStrength %d",
						 (int) rc->strength);
					break;
				case LCS_FORKEYSHARE:
					appendContextKeyword(context, " FOR KEY SHARE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORSHARE:
					appendContextKeyword(context, " FOR SHARE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORNOKEYUPDATE:
					appendContextKeyword(context, " FOR NO KEY UPDATE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
				case LCS_FORUPDATE:
					appendContextKeyword(context, " FOR UPDATE",
										 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
					break;
			}

			appendStringInfo(buf, " OF %s",
							 quote_identifier(get_rtable_name(rc->rti,
															  context)));
			if (rc->waitPolicy == LockWaitError)
				appendStringInfoString(buf, " NOWAIT");
			else if (rc->waitPolicy == LockWaitSkip)
				appendStringInfoString(buf, " SKIP LOCKED");
		}
	}
}

/*
 * 检测查询是否形如 SELECT ... FROM VALUES()，
 * 且无需重命名 VALUES RTE 的输出列。
 * 若是则返回 VALUES RTE，否则返回 NULL。
 */
static RangeTblEntry *
get_simple_values_rte(Query *query, TupleDesc resultDesc)
{
	RangeTblEntry *result = NULL;
	ListCell   *lc;

	/*
	 * 即使 Query 还包含 OLD 或 NEW 规则 RTE，我们也想
	 * 检测到匹配。所以思路是扫描 rtable，看是否只有
	 * 一个 inFromCl 且是 VALUES RTE。
	 */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		if (rte->rtekind == RTE_VALUES && rte->inFromCl)
		{
			if (result)
				return NULL;	/* 多个 VALUES（多半不可能） */
			result = rte;
		}
		else if (rte->rtekind == RTE_RELATION && !rte->inFromCl)
			continue;			/* 忽略规则项 */
		else
			return NULL;		/* 别的 -> 非简单 VALUES */
	}

	/*
	 * 我们无需过细地检查目标列表，因为
	 * parser/analyze.c 永远不会生成 "bare" VALUES RTE ---
	 * 它们只出现在结构受限的自动生成子查询中。
	 * 但 DefineView 可能已通过注入新列别名改动了 tlist
	 * 或 resultDesc 强加了别的列别名。只有当 RTE 的
	 *
	 * 列名与 get_target_list() 会选的名字一致才能简化。
	 */
	if (result)
	{
		ListCell   *lcn;
		int			colno;

		if (list_length(query->targetList) != list_length(result->eref->colnames))
			return NULL;		/* 这多半不会发生 */
		colno = 0;
		forboth(lc, query->targetList, lcn, result->eref->colnames)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			char	   *cname = strVal(lfirst(lcn));
			char	   *colname;

			if (tle->resjunk)
				return NULL;	/* 这多半不会发生 */

			/* 计算 get_target_list 会为该列使用的名字 */
			colno++;
			if (resultDesc && colno <= resultDesc->natts)
				colname = NameStr(TupleDescAttr(resultDesc, colno - 1)->attname);
			else
				colname = tle->resname;

			/* 它与 VALUES RTE 匹配吗？*/
			if (colname == NULL || strcmp(colname, cname) != 0)
				return NULL;	/* 列名已被改动 */
		}
	}

	return result;
}

static void
get_basic_select_query(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *values_rte;
	char	   *sep;
	ListCell   *l;

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}

	/*
	 * 若查询形如 SELECT * FROM (VALUES ...)，则只打印
	 * VALUES 部分。这逆转 transformValuesClause() 在解析时所做的
	 *
	 */
	values_rte = get_simple_values_rte(query, context->resultDesc);
	if (values_rte)
	{
		get_values_def(values_rte->values_lists, context);
		return;
	}

	/*
	 * 开始构建查询字符串 - 首先写 SELECT
	 */
	if (query->isReturn)
		appendStringInfoString(buf, "RETURN");
	else
		appendStringInfoString(buf, "SELECT");

	/* 若有 DISTINCT 子句则添加 */
	if (query->distinctClause != NIL)
	{
		if (query->hasDistinctOn)
		{
			appendStringInfoString(buf, " DISTINCT ON (");
			sep = "";
			foreach(l, query->distinctClause)
			{
				SortGroupClause *srt = (SortGroupClause *) lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_sortgroupclause(srt->tleSortGroupRef, query->targetList,
										 false, context);
				sep = ", ";
			}
			appendStringInfoChar(buf, ')');
		}
		else
			appendStringInfoString(buf, " DISTINCT");
	}

	/* 然后说明选择什么（目标列表） */
	get_target_list(query->targetList, context);

	/* 如需要则添加 FROM 子句*/
	get_from_clause(query, " FROM ", context);

	/* 若有 WHERE 子句则添加 */
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/* 若有 GROUP BY 子句则添加 */
	if (query->groupClause != NULL || query->groupingSets != NULL)
	{
		bool		save_ingroupby;

		appendContextKeyword(context, " GROUP BY ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		if (query->groupDistinct)
			appendStringInfoString(buf, "DISTINCT ");

		save_ingroupby = context->inGroupBy;
		context->inGroupBy = true;

		if (query->groupingSets == NIL)
		{
			sep = "";
			foreach(l, query->groupClause)
			{
				SortGroupClause *grp = (SortGroupClause *) lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_sortgroupclause(grp->tleSortGroupRef, query->targetList,
										 false, context);
				sep = ", ";
			}
		}
		else
		{
			sep = "";
			foreach(l, query->groupingSets)
			{
				GroupingSet *grp = lfirst(l);

				appendStringInfoString(buf, sep);
				get_rule_groupingset(grp, query->targetList, true, context);
				sep = ", ";
			}
		}

		context->inGroupBy = save_ingroupby;
	}

	/* 若有 HAVING 子句则添加 */
	if (query->havingQual != NULL)
	{
		appendContextKeyword(context, " HAVING ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 0);
		get_rule_expr(query->havingQual, context, false);
	}

	/* 如需要则添加 WINDOW 子句*/
	if (query->windowClause != NIL)
		get_rule_windowclause(query, context);
}

/* ----------
 * get_target_list			- 反解析 SELECT 目标列表
 *
 * 也用于 INSERT/UPDATE/DELETE 的 RETURNING 列表。
 * ----------
 */
static void
get_target_list(List *targetList, deparse_context *context)
{
	StringInfo	buf = context->buf;
	StringInfoData targetbuf;
	bool		last_was_multiline = false;
	char	   *sep;
	int			colno;
	ListCell   *l;

	/* 我们用 targetbuf 暂时存放每个 TLE 的文本 */
	initStringInfo(&targetbuf);

	sep = " ";
	colno = 0;
	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		char	   *colname;
		char	   *attname;

		if (tle->resjunk)
			continue;			/* 忽略垃圾项 */

		appendStringInfoString(buf, sep);
		sep = ", ";
		colno++;

		/*
		 * 把新字段文本放入 targetbuf，以便在取得它后再决
		 * 定它是否需要另起一行。
		 */
		resetStringInfo(&targetbuf);
		context->buf = &targetbuf;

		/*
		 * 我们特殊处理 Var 节点而不使用 get_rule_expr。这是
		 * 因为 get_rule_expr 会把整行 Var 显示成 "foo.*"，
		 * 这在多数上下文中是首选记法，但在 SELECT 列表的
		 * 顶层却不行（解析器会把它展开成多列，产生
		 * 与整行 Var 不同的行为）。
		 * 我们需要直接调用 get_variable，好让它
		 * 做正确的事，也让我们能取得作为默认 AS 标签的
		 * 属性名。
		 */
		if (tle->expr && (IsA(tle->expr, Var)))
		{
			attname = get_variable((Var *) tle->expr, 0, true, context);
		}
		else
		{
			get_rule_expr((Node *) tle->expr, context, true);

			/*
			 * 当 colNamesVisible 为真时，我们总应显式
			 * 显示已分配的列名。否则，仅当名字不是
			 * FigureColname 兜底时显示。
			 */
			attname = context->colNamesVisible ? NULL : "?column?";
		}

		/*
		 * 弄清结果列应该叫什么。在视图的上下文中，
		 * 使用视图的元组描述符（以便体现对视图做过的
		 * 任何列 RENAME 的效果）。
		 * 否则就直接使用能从 TLE 中找到的。
		 */
		if (context->resultDesc && colno <= context->resultDesc->natts)
			colname = NameStr(TupleDescAttr(context->resultDesc,
											colno - 1)->attname);
		else
			colname = tle->resname;

		/* 除非列名本来就正确，否则显示 AS */
		if (colname)			/* resname 可空 */
		{
			if (attname == NULL || strcmp(attname, colname) != 0)
				appendStringInfo(&targetbuf, " AS %s", quote_identifier(colname));
		}

		/*恢复上下文的输出缓冲区*/
		context->buf = buf;

		/* 若启用则考虑换行 */
		if (PRETTY_INDENT(context) && context->wrapColumn >= 0)
		{
			int			leading_nl_pos;

			/* 新字段是否以新行开始？ */
			if (targetbuf.len > 0 && targetbuf.data[0] == '\n')
				leading_nl_pos = 0;
			else
				leading_nl_pos = -1;

			/* 若是，则不应再加东西 */
			if (leading_nl_pos >= 0)
			{
				/* 而是去掉 buf 中现有的尾部空格 */
				removeStringInfoSpaces(buf);
			}
			else
			{
				char	   *trailing_nl;

				/* 在输出缓冲区中定位当前行的起点 */
				trailing_nl = strrchr(buf->data, '\n');
				if (trailing_nl == NULL)
					trailing_nl = buf->data;
				else
					trailing_nl++;

				/*
				 * 若新字段不是第一个，且新字段会致溢出，
				 * 或上一字段占多行，则加换行与缩进。
				 *
				 */
				if (colno > 1 &&
					((strlen(trailing_nl) + targetbuf.len > context->wrapColumn) ||
					 last_was_multiline))
					appendContextKeyword(context, "", -PRETTYINDENT_STD,
										 PRETTYINDENT_STD, PRETTYINDENT_VAR);
			}

			/* 记住本字段的多行状态供下次迭代使用 */
			last_was_multiline =
				(strchr(targetbuf.data + leading_nl_pos + 1, '\n') != NULL);
		}

		/* 添加新字段 */
		appendBinaryStringInfo(buf, targetbuf.data, targetbuf.len);
	}

	/* 清理 */
	pfree(targetbuf.data);
}

static void
get_setop_query(Node *setOp, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		need_paren;

	/* 防范过长或嵌套过深的查询 */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	if (IsA(setOp, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) setOp;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);
		Query	   *subquery = rte->subquery;

		Assert(subquery != NULL);

		/*
		 * 若有 WITH、ORDER BY、FOR UPDATE 或 LIMIT 则需要
		 * 括号（见 gram.y）。叶子查询自身含集合操作时要
		 * 括号。（除非其他子句在场，否则不应出现这
		 * 情况，见 transformSetOperationTree；但求稳。）
		 */
		need_paren = (subquery->cteList ||
					  subquery->sortClause ||
					  subquery->rowMarks ||
					  subquery->limitOffset ||
					  subquery->limitCount ||
					  subquery->setOperations);
		if (need_paren)
			appendStringInfoChar(buf, '(');
		get_query_def(subquery, buf, context->namespaces,
					  context->resultDesc, context->colNamesVisible,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
		if (need_paren)
			appendStringInfoChar(buf, ')');
	}
	else if (IsA(setOp, SetOperationStmt))
	{
		SetOperationStmt *op = (SetOperationStmt *) setOp;
		int			subindent;
		bool		save_colnamesvisible;

		/*
		 * 嵌套两个 SetOperationStmt 时我们强制加括号，除非
		 * 左输入是同类 setop。从语法上说，我们本可以在
		 * 更多情形下省略括号，但最好还是用括号来标记
		 * 集合操作算子变化的情形。若我们用了括号，还
		 * 会同时增加子查询的缩进级别。
		 *
		 * 还有些情形需要对叶子查询也加括号，但这些在
		 * 下一层处理更方便（见上面代码）。
		 *
		 */
		if (IsA(op->larg, SetOperationStmt))
		{
			SetOperationStmt *lop = (SetOperationStmt *) op->larg;

			if (op->op == lop->op && op->all == lop->all)
				need_paren = false;
			else
				need_paren = true;
		}
		else
			need_paren = false;

		if (need_paren)
		{
			appendStringInfoChar(buf, '(');
			subindent = PRETTYINDENT_STD;
			appendContextKeyword(context, "", subindent, 0, 0);
		}
		else
			subindent = 0;

		get_setop_query(op->larg, query, context);

		if (need_paren)
			appendContextKeyword(context, ") ", -subindent, 0, 0);
		else if (PRETTY_INDENT(context))
			appendContextKeyword(context, "", -subindent, 0, 0);
		else
			appendStringInfoChar(buf, ' ');

		switch (op->op)
		{
			case SETOP_UNION:
				appendStringInfoString(buf, "UNION ");
				break;
			case SETOP_INTERSECT:
				appendStringInfoString(buf, "INTERSECT ");
				break;
			case SETOP_EXCEPT:
				appendStringInfoString(buf, "EXCEPT ");
				break;
			default:
				elog(ERROR, "unrecognized set op: %d",
					 (int) op->op);
		}
		if (op->all)
			appendStringInfoString(buf, "ALL ");

		/* 若 RHS 是另一 setop 则总是加括号 */
		need_paren = IsA(op->rarg, SetOperationStmt);

		/*
		 * 这里的缩进代码有意与左输入的缩进代码略有
		 * 不同，因为我们希望换行出现在不同位置。
		 *
		 */
		if (need_paren)
		{
			appendStringInfoChar(buf, '(');
			subindent = PRETTYINDENT_STD;
		}
		else
			subindent = 0;
		appendContextKeyword(context, "", subindent, 0, 0);

		/*
		 * RHS 子选择的输出列名无关紧要。
		 */
		save_colnamesvisible = context->colNamesVisible;
		context->colNamesVisible = false;

		get_setop_query(op->rarg, query, context);

		context->colNamesVisible = save_colnamesvisible;

		if (PRETTY_INDENT(context))
			context->indentLevel -= subindent;
		if (need_paren)
			appendContextKeyword(context, ")", 0, 0, 0);
	}
	else
	{
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(setOp));
	}
}

/*
 * 显示排序/分组子句。
 *
 * 同时返回表达式树，调用方无需再找一遍。
 */
static Node *
get_rule_sortgroupclause(Index ref, List *tlist, bool force_colno,
						 deparse_context *context)
{
	StringInfo	buf = context->buf;
	TargetEntry *tle;
	Node	   *expr;

	tle = get_sortgroupref_tle(ref, tlist);
	expr = (Node *) tle->expr;

	/*
	 * 若调用方要求则用列号形式。否则，若表达式是
	 * 常量，则强制以显式类型转换修饰来输出 --- 因为
	 * 简单整数常量若不加任何修饰输出会有歧义
	 * （并且会被 findTargetlistEntrySQL92() 误解），如果我们
	 * 不加修饰地输出它。类似地，如果它只是 Var，
	 * 那么若列名在 SELECT 列表中被重新指派，也有被误
	 * 解的风险，所以可能需要强制表限定。还有，如果
	 * 它比简单 Var 更复杂，则强制在其周围加额外括号，
	 * 以确保它不会被误解为 cube() 或 rollup() 结构。
	 *
	 */
	if (force_colno)
	{
		Assert(!tle->resjunk);
		appendStringInfo(buf, "%d", tle->resno);
	}
	else if (!expr)
		 /* 什么都不做，多半不可能 */ ;
	else if (IsA(expr, Const))
		get_const_expr((Const *) expr, context, 1);
	else if (IsA(expr, Var))
	{
		/* 让 get_variable 检查名字冲突 */
		bool		save_varinorderby = context->varInOrderBy;

		context->varInOrderBy = true;
		(void) get_variable((Var *) expr, 0, false, context);
		context->varInOrderBy = save_varinorderby;
	}
	else
	{
		/*
		 * 对类函数表达式我们必须强制加括号，即使
		 * PRETTY_PAREN 关闭也得加，因其正是有误读危
		 * 险的表达式。对其他表达式，仅当 PRETTY_PAREN
		 * 开启时才需强制，否则表达式自己会输出括号。
		 * （括号不能省。）
		 */
		bool		need_paren = (PRETTY_PAREN(context)
								  || IsA(expr, FuncExpr)
								  || IsA(expr, Aggref)
								  || IsA(expr, WindowFunc)
								  || IsA(expr, JsonConstructorExpr));

		if (need_paren)
			appendStringInfoChar(context->buf, '(');
		get_rule_expr(expr, context, true);
		if (need_paren)
			appendStringInfoChar(context->buf, ')');
	}

	return expr;
}

/*
 * 显示 GroupingSet
 */
static void
get_rule_groupingset(GroupingSet *gset, List *targetlist,
					 bool omit_parens, deparse_context *context)
{
	ListCell   *l;
	StringInfo	buf = context->buf;
	bool		omit_child_parens = true;
	char	   *sep = "";

	switch (gset->kind)
	{
		case GROUPING_SET_EMPTY:
			appendStringInfoString(buf, "()");
			return;

		case GROUPING_SET_SIMPLE:
			{
				if (!omit_parens || list_length(gset->content) != 1)
					appendStringInfoChar(buf, '(');

				foreach(l, gset->content)
				{
					Index		ref = lfirst_int(l);

					appendStringInfoString(buf, sep);
					get_rule_sortgroupclause(ref, targetlist,
											 false, context);
					sep = ", ";
				}

				if (!omit_parens || list_length(gset->content) != 1)
					appendStringInfoChar(buf, ')');
			}
			return;

		case GROUPING_SET_ROLLUP:
			appendStringInfoString(buf, "ROLLUP(");
			break;
		case GROUPING_SET_CUBE:
			appendStringInfoString(buf, "CUBE(");
			break;
		case GROUPING_SET_SETS:
			appendStringInfoString(buf, "GROUPING SETS (");
			omit_child_parens = false;
			break;
	}

	foreach(l, gset->content)
	{
		appendStringInfoString(buf, sep);
		get_rule_groupingset(lfirst(l), targetlist, omit_child_parens, context);
		sep = ", ";
	}

	appendStringInfoChar(buf, ')');
}

/*
 * 显示 ORDER BY 列表。
 */
static void
get_rule_orderby(List *orderList, List *targetList,
				 bool force_colno, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	sep = "";
	foreach(l, orderList)
	{
		SortGroupClause *srt = (SortGroupClause *) lfirst(l);
		Node	   *sortexpr;
		Oid			sortcoltype;
		TypeCacheEntry *typentry;

		appendStringInfoString(buf, sep);
		sortexpr = get_rule_sortgroupclause(srt->tleSortGroupRef, targetList,
											force_colno, context);
		sortcoltype = exprType(sortexpr);
		/* 查看操作符是否为该类型的默认 < 或 > */
		typentry = lookup_type_cache(sortcoltype,
									 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
		if (srt->sortop == typentry->lt_opr)
		{
			/*ASC 是默认，故不输出任何内容*/
			if (srt->nulls_first)
				appendStringInfoString(buf, " NULLS FIRST");
		}
		else if (srt->sortop == typentry->gt_opr)
		{
			appendStringInfoString(buf, " DESC");
			/* DESC 默认为 NULLS FIRST */
			if (!srt->nulls_first)
				appendStringInfoString(buf, " NULLS LAST");
		}
		else
		{
			appendStringInfo(buf, " USING %s",
							 generate_operator_name(srt->sortop,
													sortcoltype,
													sortcoltype));
			/* 明确指定以消除歧义 */
			if (srt->nulls_first)
				appendStringInfoString(buf, " NULLS FIRST");
			else
				appendStringInfoString(buf, " NULLS LAST");
		}
		sep = ", ";
	}
}

/*
 * 显示 WINDOW 子句。
 *
 * 注意 windowClause 列表可能只含匿名窗口定义，
 * 此时这里应什么都不打印。
 */
static void
get_rule_windowclause(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	const char *sep;
	ListCell   *l;

	sep = NULL;
	foreach(l, query->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->name == NULL)
			continue;			/* 忽略匿名窗口 */

		if (sep == NULL)
			appendContextKeyword(context, " WINDOW ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		else
			appendStringInfoString(buf, sep);

		appendStringInfo(buf, "%s AS ", quote_identifier(wc->name));

		get_rule_windowspec(wc, query->targetList, context);

		sep = ", ";
	}
}

/*
 * 显示窗口定义
 */
static void
get_rule_windowspec(WindowClause *wc, List *targetList,
					deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		needspace = false;
	const char *sep;
	ListCell   *l;

	appendStringInfoChar(buf, '(');
	if (wc->refname)
	{
		appendStringInfoString(buf, quote_identifier(wc->refname));
		needspace = true;
	}
	/* 分区子句总是被继承，所以只在无 refname 时打印 */
	if (wc->partitionClause && !wc->refname)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		appendStringInfoString(buf, "PARTITION BY ");
		sep = "";
		foreach(l, wc->partitionClause)
		{
			SortGroupClause *grp = (SortGroupClause *) lfirst(l);

			appendStringInfoString(buf, sep);
			get_rule_sortgroupclause(grp->tleSortGroupRef, targetList,
									 false, context);
			sep = ", ";
		}
		needspace = true;
	}
	/* 仅在不被继承时才打印排序子句 */
	if (wc->orderClause && !wc->copiedOrder)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		appendStringInfoString(buf, "ORDER BY ");
		get_rule_orderby(wc->orderClause, targetList, false, context);
		needspace = true;
	}
	/* 帧子句从不被继承，故除非是默认否则都打印 */
	if (wc->frameOptions & FRAMEOPTION_NONDEFAULT)
	{
		if (needspace)
			appendStringInfoChar(buf, ' ');
		if (wc->frameOptions & FRAMEOPTION_RANGE)
			appendStringInfoString(buf, "RANGE ");
		else if (wc->frameOptions & FRAMEOPTION_ROWS)
			appendStringInfoString(buf, "ROWS ");
		else if (wc->frameOptions & FRAMEOPTION_GROUPS)
			appendStringInfoString(buf, "GROUPS ");
		else
			Assert(false);
		if (wc->frameOptions & FRAMEOPTION_BETWEEN)
			appendStringInfoString(buf, "BETWEEN ");
		if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING)
			appendStringInfoString(buf, "UNBOUNDED PRECEDING ");
		else if (wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW)
			appendStringInfoString(buf, "CURRENT ROW ");
		else if (wc->frameOptions & FRAMEOPTION_START_OFFSET)
		{
			get_rule_expr(wc->startOffset, context, false);
			if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING)
				appendStringInfoString(buf, " PRECEDING ");
			else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING)
				appendStringInfoString(buf, " FOLLOWING ");
			else
				Assert(false);
		}
		else
			Assert(false);
		if (wc->frameOptions & FRAMEOPTION_BETWEEN)
		{
			appendStringInfoString(buf, "AND ");
			if (wc->frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING)
				appendStringInfoString(buf, "UNBOUNDED FOLLOWING ");
			else if (wc->frameOptions & FRAMEOPTION_END_CURRENT_ROW)
				appendStringInfoString(buf, "CURRENT ROW ");
			else if (wc->frameOptions & FRAMEOPTION_END_OFFSET)
			{
				get_rule_expr(wc->endOffset, context, false);
				if (wc->frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING)
					appendStringInfoString(buf, " PRECEDING ");
				else if (wc->frameOptions & FRAMEOPTION_END_OFFSET_FOLLOWING)
					appendStringInfoString(buf, " FOLLOWING ");
				else
					Assert(false);
			}
			else
				Assert(false);
		}
		if (wc->frameOptions & FRAMEOPTION_EXCLUDE_CURRENT_ROW)
			appendStringInfoString(buf, "EXCLUDE CURRENT ROW ");
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_GROUP)
			appendStringInfoString(buf, "EXCLUDE GROUP ");
		else if (wc->frameOptions & FRAMEOPTION_EXCLUDE_TIES)
			appendStringInfoString(buf, "EXCLUDE TIES ");
		/* 现在末尾会有一个空格；把它去掉*/
		buf->len--;
	}
	appendStringInfoChar(buf, ')');
}

/* ----------
 * get_insert_query_def			- 反解析 INSERT 解析树
 * ----------
 */
static void
get_insert_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *select_rte = NULL;
	RangeTblEntry *values_rte = NULL;
	RangeTblEntry *rte;
	char	   *sep;
	ListCell   *l;
	List	   *strippedexprs;

	/* 若有 WITH 子句则插入 */
	get_with_clause(query, context);

	/*
	 * 若是 INSERT ... SELECT 或多行 VALUES，则会有一
	 * 个 SELECT 或 VALUES 的 RTE。纯 VALUES 则两者皆无
	 */
	foreach(l, query->rtable)
	{
		rte = (RangeTblEntry *) lfirst(l);

		if (rte->rtekind == RTE_SUBQUERY)
		{
			if (select_rte)
				elog(ERROR, "too many subquery RTEs in INSERT");
			select_rte = rte;
		}

		if (rte->rtekind == RTE_VALUES)
		{
			if (values_rte)
				elog(ERROR, "too many values RTEs in INSERT");
			values_rte = rte;
		}
	}
	if (select_rte && values_rte)
		elog(ERROR, "both subquery and values RTEs in INSERT");

	/*
	 * 查询以 INSERT INTO relname 开头
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);

	if (PRETTY_INDENT(context))
	{
		context->indentLevel += PRETTYINDENT_STD;
		appendStringInfoChar(buf, ' ');
	}
	appendStringInfo(buf, "INSERT INTO %s",
					 generate_relation_name(rte->relid, NIL));

	/* 如需要则打印关系别名；INSERT 要求显式 AS */
	get_rte_alias(rte, query->resultRelation, true, context);

	/* 此处总要有空格 */
	appendStringInfoChar(buf, ' ');

	/*
	 * 添加插入列名列表。列名上所需的任何间接
	 * 修饰都可以从顶层目标列表推断出来。
	 */
	strippedexprs = NIL;
	sep = "";
	if (query->targetList)
		appendStringInfoChar(buf, '(');
	foreach(l, query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);

		if (tle->resjunk)
			continue;			/* 忽略垃圾项 */

		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * 输出目标列的名字；查系统目录，而不是看
		 * tle->resname，因为 resname 不会跟随 RENAME。
		 */
		appendStringInfoString(buf,
							   quote_identifier(get_attname(rte->relid,
															tle->resno,
															false)));

		/*
		 * 打印所需的任何间接（子字段或下标），并剥掉
		 * 代表间接赋值的顶层节点。
		 * 把剥出的表达式加入 strippedexprs。
		 * （若是 single-VALUES，剥出的表达式就是下面要打印
		 * 的 VALUES；否则只是 Var，并非真的有趣。
		 *
		 */
		strippedexprs = lappend(strippedexprs,
								processIndirection((Node *) tle->expr,
												   context));
	}
	if (query->targetList)
		appendStringInfoString(buf, ") ");

	if (query->override)
	{
		if (query->override == OVERRIDING_SYSTEM_VALUE)
			appendStringInfoString(buf, "OVERRIDING SYSTEM VALUE ");
		else if (query->override == OVERRIDING_USER_VALUE)
			appendStringInfoString(buf, "OVERRIDING USER VALUE ");
	}

	if (select_rte)
	{
		/* 添加 SELECT */
		get_query_def(select_rte->subquery, buf, context->namespaces, NULL,
					  false,
					  context->prettyFlags, context->wrapColumn,
					  context->indentLevel);
	}
	else if (values_rte)
	{
		/* 添加多 VALUES 表达式列表 */
		get_values_def(values_rte->values_lists, context);
	}
	else if (strippedexprs)
	{
		/* 添加 single-VALUES 表达式列表 */
		appendContextKeyword(context, "VALUES (",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
		get_rule_list_toplevel(strippedexprs, context, false);
		appendStringInfoChar(buf, ')');
	}
	else
	{
		/* 没有表达式，所以必是 DEFAULT VALUES*/
		appendStringInfoString(buf, "DEFAULT VALUES");
	}

	/*若有 ON CONFLICT 则添加*/
	if (query->onConflict)
	{
		OnConflictExpr *confl = query->onConflict;

		appendStringInfoString(buf, " ON CONFLICT");

		if (confl->arbiterElems)
		{
			/* 添加 single-VALUES 表达式列表 */
			appendStringInfoChar(buf, '(');
			get_rule_expr((Node *) confl->arbiterElems, context, false);
			appendStringInfoChar(buf, ')');

			/* 若有 WHERE 子句（部分索引用）则添加 */
			if (confl->arbiterWhere != NULL)
			{
				bool		save_varprefix;

				/*
				 * 强制 Var 不带前缀，因为解析器假定它们属于
				 * 目标关系。WHERE 子句不用 InferenceElem，
				 * 所以这里需要单独处理。
				 */
				save_varprefix = context->varprefix;
				context->varprefix = false;

				appendContextKeyword(context, " WHERE ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
				get_rule_expr(confl->arbiterWhere, context, false);

				context->varprefix = save_varprefix;
			}
		}
		else if (OidIsValid(confl->constraint))
		{
			char	   *constraint = get_constraint_name(confl->constraint);

			if (!constraint)
				elog(ERROR, "cache lookup failed for constraint %u",
					 confl->constraint);
			appendStringInfo(buf, " ON CONSTRAINT %s",
							 quote_identifier(constraint));
		}

		if (confl->action == ONCONFLICT_NOTHING)
		{
			appendStringInfoString(buf, " DO NOTHING");
		}
		else
		{
			appendStringInfoString(buf, " DO UPDATE SET ");
			/*反解析 targetlist*/
			get_update_query_targetlist_def(query, confl->onConflictSet,
											context, rte);

			/* 若有 WHERE 子句则添加*/
			if (confl->onConflictWhere != NULL)
			{
				appendContextKeyword(context, " WHERE ",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
				get_rule_expr(confl->onConflictWhere, context, false);
			}
		}
	}

	/*若有 RETURNING 则添加*/
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context);
	}
}


/* ----------
 * get_update_query_def			- 反解析 UPDATE 解析树
 * ----------
 */
static void
get_update_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;

	/* 若有 WITH 子句则插入 */
	get_with_clause(query, context);

	/*
	 * 查询以 UPDATE relname SET 开头
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "UPDATE %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* 如需要则打印关系别名 */
	get_rte_alias(rte, query->resultRelation, false, context);

	appendStringInfoString(buf, " SET ");

	/*反解析 targetlist*/
	get_update_query_targetlist_def(query, query->targetList, context, rte);

	/* 如需要则添加 FROM 子句*/
	get_from_clause(query, " FROM ", context);

	/* 若有 WHERE 子句则添加*/
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/*若有 RETURNING 则添加*/
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context);
	}
}


/* ----------
 * get_update_query_targetlist_def			- 反解析 UPDATE 目标列表
 * ----------
 */
static void
get_update_query_targetlist_def(Query *query, List *targetList,
								deparse_context *context, RangeTblEntry *rte)
{
	StringInfo	buf = context->buf;
	ListCell   *l;
	ListCell   *next_ma_cell;
	int			remaining_ma_columns;
	const char *sep;
	SubLink    *cur_ma_sublink;
	List	   *ma_sublinks;

	/*
	 * 准备处理 MULTIEXPR 赋值：把来源 SubLink 收集成列
	 * 表。我们期望它们在 resjunk 目标列表项中按 ID
	 * 排列
	 */
	ma_sublinks = NIL;
	if (query->hasSubLinks)		/* 否则不可能有 */
	{
		foreach(l, targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(l);

			if (tle->resjunk && IsA(tle->expr, SubLink))
			{
				SubLink    *sl = (SubLink *) tle->expr;

				if (sl->subLinkType == MULTIEXPR_SUBLINK)
				{
					ma_sublinks = lappend(ma_sublinks, sl);
					Assert(sl->subLinkId == list_length(ma_sublinks));
				}
			}
		}
	}
	next_ma_cell = list_head(ma_sublinks);
	cur_ma_sublink = NULL;
	remaining_ma_columns = 0;

	/* 添加逗号分隔的 'attname = value' 列表 */
	sep = "";
	foreach(l, targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(l);
		Node	   *expr;

		if (tle->resjunk)
			continue;			/* 忽略垃圾项 */

		/* 输出分隔符（无论是否处于多赋值中都可行）*/
		appendStringInfoString(buf, sep);
		sep = ", ";

		/*
		 * 检查是否开始多赋值组：若是，则
		 * 输出左括号。
		 */
		if (next_ma_cell != NULL && cur_ma_sublink == NULL)
		{
			/*
			 * 我们必须深入 expr 内部，看它是否是
			 * PARAM_MULTIEXPR Param。可能埋在 FieldStore
			 * SubscriptingRefs 与 CoerceToDomains 之下（参见
			 * processIndirection()），下面还可能有隐式转换。
			 * 反正我们会忽略隐式转换，所以不需要像
			 * processIndirection() 那样小心地跳过隐式
			 * CoerceToDomains。
			 */
			expr = (Node *) tle->expr;
			while (expr)
			{
				if (IsA(expr, FieldStore))
				{
					FieldStore *fstore = (FieldStore *) expr;

					expr = (Node *) linitial(fstore->newvals);
				}
				else if (IsA(expr, SubscriptingRef))
				{
					SubscriptingRef *sbsref = (SubscriptingRef *) expr;

					if (sbsref->refassgnexpr == NULL)
						break;

					expr = (Node *) sbsref->refassgnexpr;
				}
				else if (IsA(expr, CoerceToDomain))
				{
					CoerceToDomain *cdomain = (CoerceToDomain *) expr;

					if (cdomain->coercionformat != COERCE_IMPLICIT_CAST)
						break;
					expr = (Node *) cdomain->arg;
				}
				else
					break;
			}
			expr = strip_implicit_coercions(expr);

			if (expr && IsA(expr, Param) &&
				((Param *) expr)->paramkind == PARAM_MULTIEXPR)
			{
				cur_ma_sublink = (SubLink *) lfirst(next_ma_cell);
				next_ma_cell = lnext(ma_sublinks, next_ma_cell);
				remaining_ma_columns = count_nonjunk_tlist_entries(((Query *) cur_ma_sublink->subselect)->targetList);
				Assert(((Param *) expr)->paramid ==
					   ((cur_ma_sublink->subLinkId << 16) | 1));
				appendStringInfoChar(buf, '(');
			}
		}

		/*
		 * 输出目标列的名字；查系统目录，而不是看
		 * tle->resname，因为 resname 不会跟随 RENAME。
		 */
		appendStringInfoString(buf,
							   quote_identifier(get_attname(rte->relid,
															tle->resno,
															false)));

		/*
		 * 打印所需的任何间接（子字段或下标），并剥掉
		 * 代表间接赋值的顶层节点。
		 */
		expr = processIndirection((Node *) tle->expr, context);

		/*
		 * 若处于多赋值中，则跳过不再打印任何内容，除非
		 * 这是最后一列；此时应打印的是 sublink 而
		 * 不是 Param。
		 */
		if (cur_ma_sublink != NULL)
		{
			if (--remaining_ma_columns > 0)
				continue;		/* 非多赋值的最后一列 */
			appendStringInfoChar(buf, ')');
			expr = (Node *) cur_ma_sublink;
			cur_ma_sublink = NULL;
		}

		appendStringInfoString(buf, " = ");

		get_rule_expr(expr, context, false);
	}
}


/* ----------
 * get_delete_query_def			- 反解析 DELETE 解析树
 * ----------
 */
static void
get_delete_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;

	/* 若有 WITH 子句则插入 */
	get_with_clause(query, context);

	/*
	 * 查询以 DELETE FROM relname 开头
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "DELETE FROM %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* 如需要则打印关系别名 */
	get_rte_alias(rte, query->resultRelation, false, context);

	/* 若有 USING 子句则添加 */
	get_from_clause(query, " USING ", context);

	/* 若有 WHERE 子句则添加*/
	if (query->jointree->quals != NULL)
	{
		appendContextKeyword(context, " WHERE ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_rule_expr(query->jointree->quals, context, false);
	}

	/*若有 RETURNING 则添加*/
	if (query->returningList)
	{
		appendContextKeyword(context, " RETURNING",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 1);
		get_target_list(query->returningList, context);
	}
}


/* ----------
 * get_merge_query_def				- 反解析 MERGE 解析树
 * ----------
 */
static void
get_merge_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;
	ListCell   *lc;

	/* 若有 WITH 子句则插入 */
	get_with_clause(query, context);

	/*
	 * 查询以 MERGE INTO relname 开头
	 */
	rte = rt_fetch(query->resultRelation, query->rtable);
	Assert(rte->rtekind == RTE_RELATION);
	if (PRETTY_INDENT(context))
	{
		appendStringInfoChar(buf, ' ');
		context->indentLevel += PRETTYINDENT_STD;
	}
	appendStringInfo(buf, "MERGE INTO %s%s",
					 only_marker(rte),
					 generate_relation_name(rte->relid, NIL));

	/* 如需要则打印关系别名 */
	get_rte_alias(rte, query->resultRelation, false, context);

	/* 打印源关系与连接子句 */
	get_from_clause(query, " USING ", context);
	appendContextKeyword(context, " ON ",
						 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
	get_rule_expr(query->jointree->quals, context, false);

	/*打印每个 merge 动作*/
	foreach(lc, query->mergeActionList)
	{
		MergeAction *action = lfirst_node(MergeAction, lc);

		appendContextKeyword(context, " WHEN ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
		appendStringInfo(buf, "%sMATCHED", action->matched ? "" : "NOT ");

		if (action->qual)
		{
			appendContextKeyword(context, " AND ",
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 3);
			get_rule_expr(action->qual, context, false);
		}
		appendContextKeyword(context, " THEN ",
							 -PRETTYINDENT_STD, PRETTYINDENT_STD, 3);

		if (action->commandType == CMD_INSERT)
		{
			/* 这大体上与 get_insert_query_def() 一致 */
			List	   *strippedexprs = NIL;
			const char *sep = "";
			ListCell   *lc2;

			appendStringInfoString(buf, "INSERT");

			if (action->targetList)
				appendStringInfoString(buf, " (");
			foreach(lc2, action->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc2);

				Assert(!tle->resjunk);

				appendStringInfoString(buf, sep);
				sep = ", ";

				appendStringInfoString(buf,
									   quote_identifier(get_attname(rte->relid,
																	tle->resno,
																	false)));
				strippedexprs = lappend(strippedexprs,
										processIndirection((Node *) tle->expr,
														   context));
			}
			if (action->targetList)
				appendStringInfoChar(buf, ')');

			if (action->override)
			{
				if (action->override == OVERRIDING_SYSTEM_VALUE)
					appendStringInfoString(buf, " OVERRIDING SYSTEM VALUE");
				else if (action->override == OVERRIDING_USER_VALUE)
					appendStringInfoString(buf, " OVERRIDING USER VALUE");
			}

			if (strippedexprs)
			{
				appendContextKeyword(context, " VALUES (",
									 -PRETTYINDENT_STD, PRETTYINDENT_STD, 4);
				get_rule_list_toplevel(strippedexprs, context, false);
				appendStringInfoChar(buf, ')');
			}
			else
				appendStringInfoString(buf, " DEFAULT VALUES");
		}
		else if (action->commandType == CMD_UPDATE)
		{
			appendStringInfoString(buf, "UPDATE SET ");
			get_update_query_targetlist_def(query, action->targetList,
											context, rte);
		}
		else if (action->commandType == CMD_DELETE)
			appendStringInfoString(buf, "DELETE");
		else if (action->commandType == CMD_NOTHING)
			appendStringInfoString(buf, "DO NOTHING");
	}

	/* MERGE 尚不支持 RETURNING */
	Assert(query->returningList == NIL);
}


/* ----------
 * get_utility_query_def			- 反解析 UTILITY 解析树
 * ----------
 */
static void
get_utility_query_def(Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (query->utilityStmt && IsA(query->utilityStmt, NotifyStmt))
	{
		NotifyStmt *stmt = (NotifyStmt *) query->utilityStmt;

		appendContextKeyword(context, "",
							 0, PRETTYINDENT_STD, 1);
		appendStringInfo(buf, "NOTIFY %s",
						 quote_identifier(stmt->conditionname));
		if (stmt->payload)
		{
			appendStringInfoString(buf, ", ");
			simple_quote_literal(buf, stmt->payload);
		}
	}
	else
	{
		/* 目前规则中只能出现 NOTIFY 实用命令 */
		elog(ERROR, "unexpected utility statement type");
	}
}

/*
 * 恰当地显示 Var。
 *
 * 某些情况（目前仅递归进入未命名连接时）下，
 * Var 的 varlevelsup 必须相对于当前之上的上下文
 * 来解释；levelsup 指示偏移量。
 *
 * 若 istoplevel 为真，则 Var 位于 SELECT 目标列表的
 * 顶层，这意味着我们需要对整行 Var 特殊处理。
 * 我们将不打印正常的 "tab.*"，而是打印 "tab.*::typename"，
 * 这是一个肮脏的 hack，防止 "tab.*" 被展开成多列。
 * （解析器会剥掉无用的类型转换，所以在转储与重
 * 载时不会增加低效。）过去我们在这种情况下只打
 * 印 "tab"，但这有歧义，若 "tab" 也是查询中的普通列
 * 名时会出错。
 *
 * 返回 Var 的 attname；若 Var 没有 attname（因为它是
 * 整行 Var 或子 plan 输出引用），则返回 NULL。
 */
static char *
get_variable(Var *var, int levelsup, bool istoplevel, deparse_context *context)
{
	StringInfo	buf = context->buf;
	RangeTblEntry *rte;
	AttrNumber	attnum;
	int			netlevelsup;
	deparse_namespace *dpns;
	int			varno;
	AttrNumber	varattno;
	deparse_columns *colinfo;
	char	   *refname;
	char	   *attname;
	bool		need_prefix;

	/* 找到适当的嵌套深度 */
	netlevelsup = var->varlevelsup + levelsup;
	if (netlevelsup >= list_length(context->namespaces))
		elog(ERROR, "bogus varlevelsup: %d offset %d",
			 var->varlevelsup, levelsup);
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  netlevelsup);

	/*
	 * 若我们拥有该 Var 的语法引用对象，且正在根据
	 * 解析树工作，则优先使用语法引用对象。否则退回
	 * 语义引用对象。（在打印 plan 树时强制使用语义引
	 * 用对象是一种设计选择，其动机或许更多是向后
	 * 兼容而非其他。但它确实有让 plan 更显式之效。
	 *
	 */
	if (var->varnosyn > 0 && dpns->plan == NULL)
	{
		varno = var->varnosyn;
		varattno = var->varattnosyn;
	}
	else
	{
		varno = var->varno;
		varattno = var->varattno;
	}

	/*
	 * 设法在本 rtable 中找到相关 RTE。在 plan 树中，
	 * varno 很可能是 OUTER_VAR 或 INNER_VAR，此时我们
	 * 必须深入子 plan；INDEX_VAR 也按类似方式解析。同时
	 * 还要找到之前为该 RTE 分配的别名。
	 */
	if (varno >= 1 && varno <= list_length(dpns->rtable))
	{
		/*
		 * 我们可能被要求把子 Var 映射到某个父关系。
		 */
		if (context->appendparents && dpns->appendrels)
		{
			int			pvarno = varno;
			AttrNumber	pvarattno = varattno;
			AppendRelInfo *appinfo = dpns->appendrels[pvarno];
			bool		found = false;

			/* 只映射继承父关系，不映射 UNION ALL appendrels */
			while (appinfo &&
				   rt_fetch(appinfo->parent_relid,
							dpns->rtable)->rtekind == RTE_RELATION)
			{
				found = false;
				if (pvarattno > 0)	/* 系统列保持原样 */
				{
					if (pvarattno > appinfo->num_child_cols)
						break;	/* 保险 */
					pvarattno = appinfo->parent_colnos[pvarattno - 1];
					if (pvarattno == 0)
						break;	/* Var 属子节点 */
				}

				pvarno = appinfo->parent_relid;
				found = true;

				/* 若父本身也是子，则继续向上。 */
				Assert(pvarno > 0 && pvarno <= list_length(dpns->rtable));
				appinfo = dpns->appendrels[pvarno];
			}

			/*
			 * 若找到了某个祖先关系，且该关系包含在
			 * appendparents 中，则打印该列而非原列。
			 */
			if (found && bms_is_member(pvarno, context->appendparents))
			{
				varno = pvarno;
				varattno = pvarattno;
			}
		}

		rte = rt_fetch(varno, dpns->rtable);
		refname = (char *) list_nth(dpns->rtable_names, varno - 1);
		colinfo = deparse_columns_fetch(varno, dpns);
		attnum = varattno;
	}
	else
	{
		resolve_special_varno((Node *) var, context,
							  get_special_variable, NULL);
		return NULL;
	}

	/*
	 * 规划器有时会发出引用子查询目标列表 resjunk 项
	 * 的 Var（目前仅当它选择为 SubqueryScan 或 CteScan
	 * 节点生成 "physical tlist" 时才可能发生）。虽然我
	 * 们更喜欢用子查询的别名来打印引用子查询的
	 * Var，但 resjunk 项没有别名，所以做不到。此时我
	 * 们就下钻到子 plan 并打印被引用的目标列表项的
	 * 内容。这能行得通，因为在 plan 树中，这类 Var 只
	 * 可能出现在 SubqueryScan 或 CteScan 节点中，而且我
	 * 们已把 dpns->inner_plan 设置为引用子 plan 节点。
	 */
	if ((rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE) &&
		attnum > list_length(rte->eref->colnames) &&
		dpns->inner_plan)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;

		tle = get_tle_by_resno(dpns->inner_tlist, attnum);
		if (!tle)
			elog(ERROR, "invalid attnum %d for relation \"%s\"",
				 attnum, rte->eref->aliasname);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->inner_plan, &save_dpns);

		/*
		 * 强制加括号，因为调用方很可能假定 Var 是简单
		 * 表达式。
		 */
		if (!IsA(tle->expr, Var))
			appendStringInfoChar(buf, '(');
		get_rule_expr((Node *) tle->expr, context, true);
		if (!IsA(tle->expr, Var))
			appendStringInfoChar(buf, ')');

		pop_child_plan(dpns, &save_dpns);
		return NULL;
	}

	/*
	 * 若是未命名连接，则查看别名变量的展开。若它
	 * 只是对某个输入 Var 的简单引用，则递归打印该
	 * Var 的名字。当它不是简单引用时，我们只能直接
	 * 打印不带限定的连接列名。（这只可能发生在
	 * JOIN USING 的 "dangerous" 合并列上；此前我们已
	 * 费心让这种场合下的不带限定列名保持唯一。）
	 *
	 * 这在反解析 plan 树时行不通，因为规划之后我们
	 * 不保存 joinaliasvars 列表；但 plan 树决不应包含
	 * 连接别名 Var。
	 */
	if (rte->rtekind == RTE_JOIN && rte->alias == NULL)
	{
		if (rte->joinaliasvars == NIL)
			elog(ERROR, "cannot decompile join alias var in plan tree");
		if (attnum > 0)
		{
			Var		   *aliasvar;

			aliasvar = (Var *) list_nth(rte->joinaliasvars, attnum - 1);
			/* 我们有意不在这里剥离隐式转换 */
			if (aliasvar && IsA(aliasvar, Var))
			{
				return get_variable(aliasvar, var->varlevelsup + levelsup,
									istoplevel, context);
			}
		}

		/*
		 * 未命名连接没有 refname。（注意：既然未命名，
		 * 用户就不可能引用它来创建整行 Var，所以下面的
		 * 情况就无需覆盖。）
		 */
		Assert(refname == NULL);
	}

	if (attnum == InvalidAttrNumber)
		attname = NULL;
	else if (attnum > 0)
	{
		/* 从 colinfo 结构取得要用的列名 */
		if (attnum > colinfo->num_cols)
			elog(ERROR, "invalid attnum %d for relation \"%s\"",
				 attnum, rte->eref->aliasname);
		attname = colinfo->colnames[attnum - 1];

		/*
		 * 若我们发现引用已删除列的 Var，似乎打印点什
		 * 么（什么都行）总比失败好。一般来说这不应
		 * 该发生，但过去某些涉及返回命名复合类型函数
		 * 的情形可能触发，而且或许外面还有 bug 存在。
		 *
		 */
		if (attname == NULL)
			attname = "?dropped?column?";
	}
	else
	{
		/* 系统列 - 名字固定，从目录中取得 */
		attname = get_rte_attribute_name(rte, attnum);
	}

	need_prefix = (context->varprefix || attname == NULL);

	/*
	 * 如果我们面对的是 ORDER BY（而非 GROUP BY）子句中
	 * 的普通 Var，可能需要加表名前缀以防
	 * findTargetlistEntrySQL92 把该名字误解为输出列
	 * 名。为避免不必要的前缀弄乱输出，只有当存在
	 * 与某个 SELECT tlist 项名字匹配且该 Var 与之不同
	 * 的情况时才这么做。
	 */
	if (context->varInOrderBy && !context->inGroupBy && !need_prefix)
	{
		int			colno = 0;
		ListCell   *lc;

		foreach(lc, context->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);
			char	   *colname;

			if (tle->resjunk)
				continue;		/* 忽略垃圾项 */
			colno++;

			/* 这必须与 get_target_list() 中选列名的逻辑一致 */
			if (context->resultDesc && colno <= context->resultDesc->natts)
				colname = NameStr(TupleDescAttr(context->resultDesc,
												colno - 1)->attname);
			else
				colname = tle->resname;

			if (colname && strcmp(colname, attname) == 0 &&
				!equal(var, tle->expr))
			{
				need_prefix = true;
				break;
			}
		}
	}

	if (refname && need_prefix)
	{
		appendStringInfoString(buf, quote_identifier(refname));
		appendStringInfoChar(buf, '.');
	}
	if (attname)
		appendStringInfoString(buf, quote_identifier(attname));
	else
	{
		appendStringInfoChar(buf, '*');
		if (istoplevel)
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(var->vartype,
													  var->vartypmod));
	}

	return attname;
}

/*
 * 反解析引用 OUTER_VAR、INNER_VAR 或 INDEX_VAR 的 Var。
 * 本例程实际上是 resolve_special_varno 的回调，后者
 * 负责找到正确的 TargetEntry。我们取得其中包含的
 * 表达式，只需对它做反解析，这个活可丢回给
 * get_rule_expr
 */
static void
get_special_variable(Node *node, deparse_context *context, void *callback_arg)
{
	StringInfo	buf = context->buf;

	/*
	 * 对非 Var 引用对象要强制加括号，调用方很可能
	 * 假定 Var 是简单表达式。
	 */
	if (!IsA(node, Var))
		appendStringInfoChar(buf, '(');
	get_rule_expr(node, context, true);
	if (!IsA(node, Var))
		appendStringInfoChar(buf, ')');
}

/*
 * 沿着 plan 引用追踪特殊 varno（OUTER_VAR、INNER_VAR、
 * INDEX_VAR），直到找到真正的 Var 或某种非 Var 节点；
 * 然后调用给定回调。
 */
static void
resolve_special_varno(Node *node, deparse_context *context,
					  rsv_callback callback, void *callback_arg)
{
	Var		   *var;
	deparse_namespace *dpns;

	/* 本函数递归，所以我们格外小心。 */
	check_stack_depth();

	/* 若它不是 Var，则调用回调。 */
	if (!IsA(node, Var))
	{
		(*callback) (node, context, callback_arg);
		return;
	}

	/* 找到适当的嵌套深度 */
	var = (Var *) node;
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  var->varlevelsup);

	/*
	 * 若 varno 是特殊的，则递归。（不必管 varnosyn；
	 * 既然到此，就已决定不用它）
	 */
	if (var->varno == OUTER_VAR && dpns->outer_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		Bitmapset  *save_appendparents;

		tle = get_tle_by_resno(dpns->outer_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for OUTER_VAR var: %d", var->varattno);

		/*
		 * 若我们正下行到 Append 或 MergeAppend 的第一个子
		 * 节点，就更新 appendparents。这会影响其中出现
		 * 的所有 Var 的反解析。
		 */
		save_appendparents = context->appendparents;

		if (IsA(dpns->plan, Append))
			context->appendparents = bms_union(context->appendparents,
											   ((Append *) dpns->plan)->apprelids);
		else if (IsA(dpns->plan, MergeAppend))
			context->appendparents = bms_union(context->appendparents,
											   ((MergeAppend *) dpns->plan)->apprelids);

		push_child_plan(dpns, dpns->outer_plan, &save_dpns);
		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		pop_child_plan(dpns, &save_dpns);
		context->appendparents = save_appendparents;
		return;
	}
	else if (var->varno == INNER_VAR && dpns->inner_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;

		tle = get_tle_by_resno(dpns->inner_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INNER_VAR var: %d", var->varattno);

		push_child_plan(dpns, dpns->inner_plan, &save_dpns);
		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		pop_child_plan(dpns, &save_dpns);
		return;
	}
	else if (var->varno == INDEX_VAR && dpns->index_tlist)
	{
		TargetEntry *tle;

		tle = get_tle_by_resno(dpns->index_tlist, var->varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INDEX_VAR var: %d", var->varattno);

		resolve_special_varno((Node *) tle->expr, context,
							  callback, callback_arg);
		return;
	}
	else if (var->varno < 1 || var->varno > list_length(dpns->rtable))
		elog(ERROR, "bogus varno: %d", var->varno);

	/* 非特殊。直接调用回调。 */
	(*callback) (node, context, callback_arg);
}

/*
 * 取得复合类型表达式的字段名。该表达式
 * 通常是 Var，但我们也处理其他情形。
 *
 * levelsup 是额外的偏移量，用于正确解释 Var 的
 * varlevelsup。
 * 当表达式具有命名的复合类型时，这件事相当
 * 直接了当；只需在目录中查该类型即可。不过，
 * 类型也可能是 RECORD。由于实际的表列或视图列都
 * 不允许为 RECORD 类型，所以 RECORD 类型的 Var 必须
 * 引用 JOIN 或 FUNCTION RTE，或者引用子查询输出。我
 * 们会下钻找最终定义表达式，并尽力推断字段
 * 名。失败则报错。
 *
 * 类似地，RECORD 类型的 PARAM 必须引用某个可确定
 * 复合类型的表达式。
 */
static const char *
get_name_for_var_field(Var *var, int fieldno,
					   int levelsup, deparse_context *context)
{
	RangeTblEntry *rte;
	AttrNumber	attnum;
	int			netlevelsup;
	deparse_namespace *dpns;
	int			varno;
	AttrNumber	varattno;
	TupleDesc	tupleDesc;
	Node	   *expr;

	/*
	 * 若它是由整行 Var 展开而来的 RowExpr，则使用附
	 * 着它的列名。（本可以让 get_expr_result_tupdesc()
	 * 来处理，但直接取出所需的名字要便宜得多。）
	 */
	if (IsA(var, RowExpr))
	{
		RowExpr    *r = (RowExpr *) var;

		if (fieldno > 0 && fieldno <= list_length(r->colnames))
			return strVal(list_nth(r->colnames, fieldno - 1));
	}

	/*
	 * 若它是 RECORD 类型的 Param，设法找到它引用的对
	 */
	if (IsA(var, Param))
	{
		Param	   *param = (Param *) var;
		ListCell   *ancestor_cell;

		expr = find_param_referent(param, context, &dpns, &ancestor_cell);
		if (expr)
		{
			/* 找到匹配，于是递归破解字段名 */
			deparse_namespace save_dpns;
			const char *result;

			push_ancestor_plan(dpns, ancestor_cell, &save_dpns);
			result = get_name_for_var_field((Var *) expr, fieldno,
											0, context);
			pop_ancestor_plan(dpns, &save_dpns);
			return result;
		}
	}

	/*
	 * 若它是 RECORD 类型的 Var，就必须找到该 Var 引用
	 * 对象，否则用 get_expr_result_tupdesc()
	 */
	if (!IsA(var, Var) ||
		var->vartype != RECORDOID)
	{
		tupleDesc = get_expr_result_tupdesc((Node *) var, false);
		/* 拿到 tupdesc，就可以提取字段名 */
		Assert(fieldno >= 1 && fieldno <= tupleDesc->natts);
		return NameStr(TupleDescAttr(tupleDesc, fieldno - 1)->attname);
	}

	/* 找到适当的嵌套深度 */
	netlevelsup = var->varlevelsup + levelsup;
	if (netlevelsup >= list_length(context->namespaces))
		elog(ERROR, "bogus varlevelsup: %d offset %d",
			 var->varlevelsup, levelsup);
	dpns = (deparse_namespace *) list_nth(context->namespaces,
										  netlevelsup);

	/*
	 * 若我们拥有该 Var 的语法引用对象，且正在根据
	 * 解析树工作，则优先使用语法引用对象。否则退回
	 * 语义引用对象。（见 get_variable() 内注释。）
	 */
	if (var->varnosyn > 0 && dpns->plan == NULL)
	{
		varno = var->varnosyn;
		varattno = var->varattnosyn;
	}
	else
	{
		varno = var->varno;
		varattno = var->varattno;
	}

	/*
	 * 设法在本 rtable 中找到相关 RTE。在 plan 树中，
	 * varno 很可能是 OUTER_VAR 或 INNER_VAR，此时我们
	 * 必须深入子 plan；INDEX_VAR 也按类似方式解析。
	 *
	 * 注意：与 get_variable 和 resolve_special_varno 不同，
	 * 我们无需操心继承映射：子 Var 的数据类型应与
	 * 其父相同，这里我们真正关心的只是 Var 的类型。
	 */
	if (varno >= 1 && varno <= list_length(dpns->rtable))
	{
		rte = rt_fetch(varno, dpns->rtable);
		attnum = varattno;
	}
	else if (varno == OUTER_VAR && dpns->outer_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		const char *result;

		tle = get_tle_by_resno(dpns->outer_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for OUTER_VAR var: %d", varattno);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->outer_plan, &save_dpns);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		pop_child_plan(dpns, &save_dpns);
		return result;
	}
	else if (varno == INNER_VAR && dpns->inner_tlist)
	{
		TargetEntry *tle;
		deparse_namespace save_dpns;
		const char *result;

		tle = get_tle_by_resno(dpns->inner_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INNER_VAR var: %d", varattno);

		Assert(netlevelsup == 0);
		push_child_plan(dpns, dpns->inner_plan, &save_dpns);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		pop_child_plan(dpns, &save_dpns);
		return result;
	}
	else if (varno == INDEX_VAR && dpns->index_tlist)
	{
		TargetEntry *tle;
		const char *result;

		tle = get_tle_by_resno(dpns->index_tlist, varattno);
		if (!tle)
			elog(ERROR, "bogus varattno for INDEX_VAR var: %d", varattno);

		Assert(netlevelsup == 0);

		result = get_name_for_var_field((Var *) tle->expr, fieldno,
										levelsup, context);

		return result;
	}
	else
	{
		elog(ERROR, "bogus varno: %d", varno);
		return NULL;			/* 让编译器安静 */
	}

	if (attnum == InvalidAttrNumber)
	{
		/* Var 是对 RTE 的整行引用，所以选择正确字段 */
		return get_rte_attribute_name(rte, fieldno);
	}

	/*
	 * 这部分逻辑本质上与解析器的
	 * expandRecordVariable() 相同，但我们处理的是不同的
	 * 输入上下文表示，而且我们只需要一个字段名而
	 * 不是 TupleDesc。另外，反解析 plan 树时需要特殊
	 * 情形找子查询与 CTE 子 plan。
	 */
	expr = (Node *) var;		/* 无法下钻时的默认值 */

	switch (rte->rtekind)
	{
		case RTE_RELATION:
		case RTE_VALUES:
		case RTE_NAMEDTUPLESTORE:
		case RTE_RESULT:

			/*
			 * 这情形不应出现：表列、values 列表或 ENR 的列
			 * 不应具有 RECORD 类型。直接落到底部失败（多半
			 * 如此）。
			 */
			break;
		case RTE_SUBQUERY:
			/*FROM 中的子选择：查子选择的输出表达式*/
			{
				if (rte->subquery)
				{
					TargetEntry *ste = get_tle_by_resno(rte->subquery->targetList,
														attnum);

					if (ste == NULL || ste->resjunk)
						elog(ERROR, "subquery %s does not have attribute %d",
							 rte->eref->aliasname, attnum);
					expr = (Node *) ste->expr;
					if (IsA(expr, Var))
					{
						/*
						 * 递归进子选择，看 Var 引用什么。
						 * 须额外建一层命名空间同步子选择内
						 * varlevelsup；子查询 RTE 可能来自外层，
						 * 此时子选择的命名空间必须以该外层
						 * 为父命名空间。
						 *
						 *
						 */
						List	   *save_nslist = context->namespaces;
						List	   *parent_namespaces;
						deparse_namespace mydpns;
						const char *result;

						parent_namespaces = list_copy_tail(context->namespaces,
														   netlevelsup);

						set_deparse_for_query(&mydpns, rte->subquery,
											  parent_namespaces);

						context->namespaces = lcons(&mydpns, parent_namespaces);

						result = get_name_for_var_field((Var *) expr, fieldno,
														0, context);

						context->namespaces = save_nslist;

						return result;
					}
					/* 否则落到下面检查该表达式 */
				}
				else
				{
					/*
					 * 我们在反解析 plan 树，故无完整的 RTE 项
					 * （rte->subquery 为 NULL）。不过直接引用
					 * SUBQUERY RTE 的 Var 处仅 SubqueryScan plan
					 * 节点，可改看子 plan 的 tlist。例外是
					 * 子查询被证明为空且被优化掉：会在
					 * 无子 Result 节点中见这种 Var，且
					 * plan 树中没有任何线索能弄清它当初引用
					 * 的是什么。那时退回打印 "fN"，类似
					 * 于 RowExpr 的默认列名。
					 *
					 *
					 *
					 */
					TargetEntry *tle;
					deparse_namespace save_dpns;
					const char *result;

					if (!dpns->inner_plan)
					{
						char	   *dummy_name = palloc(32);

						Assert(dpns->plan && IsA(dpns->plan, Result));
						snprintf(dummy_name, 32, "f%d", fieldno);
						return dummy_name;
					}
					Assert(dpns->plan && IsA(dpns->plan, SubqueryScan));

					tle = get_tle_by_resno(dpns->inner_tlist, attnum);
					if (!tle)
						elog(ERROR, "bogus varattno for subquery var: %d",
							 attnum);
					Assert(netlevelsup == 0);
					push_child_plan(dpns, dpns->inner_plan, &save_dpns);

					result = get_name_for_var_field((Var *) tle->expr, fieldno,
													levelsup, context);

					pop_child_plan(dpns, &save_dpns);
					return result;
				}
			}
			break;
		case RTE_JOIN:
			/* 连接 RTE --- 递归检查别名变量 */
			if (rte->joinaliasvars == NIL)
				elog(ERROR, "cannot decompile join alias var in plan tree");
			Assert(attnum > 0 && attnum <= list_length(rte->joinaliasvars));
			expr = (Node *) list_nth(rte->joinaliasvars, attnum - 1);
			Assert(expr != NULL);
			/* 我们有意不在这里剥离隐式转换 */
			if (IsA(expr, Var))
				return get_name_for_var_field((Var *) expr, fieldno,
											  var->varlevelsup + levelsup,
											  context);
			/* 否则落到下面检查该表达式 */
			break;
		case RTE_FUNCTION:
		case RTE_TABLEFUNC:

			/*
			 * 除非某函数把其结果列之一声明为 RECORD，否则
			 * 我们到不了这里，而那是不允许的。
			 */
			break;
		case RTE_CTE:
			/* CTE 引用：检查子查询的输出表达式*/
			{
				CommonTableExpr *cte = NULL;
				Index		ctelevelsup;
				ListCell   *lc;

				/*
				 * 利用命名空间栈设法找到被引用的 CTE。
				 */
				ctelevelsup = rte->ctelevelsup + netlevelsup;
				if (ctelevelsup >= list_length(context->namespaces))
					lc = NULL;
				else
				{
					deparse_namespace *ctedpns;

					ctedpns = (deparse_namespace *)
						list_nth(context->namespaces, ctelevelsup);
					foreach(lc, ctedpns->ctes)
					{
						cte = (CommonTableExpr *) lfirst(lc);
						if (strcmp(cte->ctename, rte->ctename) == 0)
							break;
					}
				}
				if (lc != NULL)
				{
					Query	   *ctequery = (Query *) cte->ctequery;
					TargetEntry *ste = get_tle_by_resno(GetCTETargetList(cte),
														attnum);

					if (ste == NULL || ste->resjunk)
						elog(ERROR, "CTE %s does not have attribute %d",
							 rte->eref->aliasname, attnum);
					expr = (Node *) ste->expr;
					if (IsA(expr, Var))
					{
						/*
						 * 递归进入 CTE，看其 Var 引用什么。
						 * 须额外建一层命名空间，以与 CTE 内 varlevelsup
						 * 保持同步；
						 * 另外它可能是外层 CTE（对比
						 * SUBQUERY 情形）。
						 */
						List	   *save_nslist = context->namespaces;
						List	   *parent_namespaces;
						deparse_namespace mydpns;
						const char *result;

						parent_namespaces = list_copy_tail(context->namespaces,
														   ctelevelsup);

						set_deparse_for_query(&mydpns, ctequery,
											  parent_namespaces);

						context->namespaces = lcons(&mydpns, parent_namespaces);

						result = get_name_for_var_field((Var *) expr, fieldno,
														0, context);

						context->namespaces = save_nslist;

						return result;
					}
					/* 否则落到下面检查该表达式 */
				}
				else
				{
					/*
					 * 反解析 plan 树时没有 CTE 列表。但
					 * 唯一会直接引用 CTE RTE 的 Var 之处是
					 * CteScan 或 WorkTableScan 节点。对它们
					 * set_deparse_plan 已让 dpns->inner_plan
					 * 是发出 CTE/RecursiveUnion 结果 plan 节点，
					 * 可改看其 tlist。同上，若 CTE 被证明
					 * 为空，那就退回到 "fN"。
					 *
					 *
					 */
					TargetEntry *tle;
					deparse_namespace save_dpns;
					const char *result;

					if (!dpns->inner_plan)
					{
						char	   *dummy_name = palloc(32);

						Assert(dpns->plan && IsA(dpns->plan, Result));
						snprintf(dummy_name, 32, "f%d", fieldno);
						return dummy_name;
					}
					Assert(dpns->plan && (IsA(dpns->plan, CteScan) ||
										  IsA(dpns->plan, WorkTableScan)));

					tle = get_tle_by_resno(dpns->inner_tlist, attnum);
					if (!tle)
						elog(ERROR, "bogus varattno for subquery var: %d",
							 attnum);
					Assert(netlevelsup == 0);
					push_child_plan(dpns, dpns->inner_plan, &save_dpns);

					result = get_name_for_var_field((Var *) tle->expr, fieldno,
													levelsup, context);

					pop_child_plan(dpns, &save_dpns);
					return result;
				}
			}
			break;
	}

	/*
	 * 现在我们有一个无法再展开的表达式，看看
	 * get_expr_result_tupdesc() 能否处理它。
	 */
	tupleDesc = get_expr_result_tupdesc(expr, false);
	/* 拿到 tupdesc，就可以提取字段名 */
	Assert(fieldno >= 1 && fieldno <= tupleDesc->natts);
	return NameStr(TupleDescAttr(tupleDesc, fieldno - 1)->attname);
}

/*
 * Try to find the referenced expression for a PARAM_EXEC Param that might
 * reference a parameter supplied by an upper NestLoop or SubPlan plan node.
 *
 * If successful, return the expression and set *dpns_p and *ancestor_cell_p
 * appropriately for calling push_ancestor_plan().  If no referent can be
 * found, return NULL.
 */
static Node *
find_param_referent(Param *param, deparse_context *context,
					deparse_namespace **dpns_p, ListCell **ancestor_cell_p)
{
	/* Initialize output parameters to prevent compiler warnings */
	*dpns_p = NULL;
	*ancestor_cell_p = NULL;

	/*
	 * If it's a PARAM_EXEC parameter, look for a matching NestLoopParam or
	 * SubPlan argument.  This will necessarily be in some ancestor of the
	 * current expression's Plan node.
	 */
	if (param->paramkind == PARAM_EXEC)
	{
		deparse_namespace *dpns;
		Plan	   *child_plan;
		ListCell   *lc;

		dpns = (deparse_namespace *) linitial(context->namespaces);
		child_plan = dpns->plan;

		foreach(lc, dpns->ancestors)
		{
			Node	   *ancestor = (Node *) lfirst(lc);
			ListCell   *lc2;

			/*
			 * NestLoops transmit params to their inner child only.
			 */
			if (IsA(ancestor, NestLoop) &&
				child_plan == innerPlan(ancestor))
			{
				NestLoop   *nl = (NestLoop *) ancestor;

				foreach(lc2, nl->nestParams)
				{
					NestLoopParam *nlp = (NestLoopParam *) lfirst(lc2);

					if (nlp->paramno == param->paramid)
					{
						/* Found a match, so return it */
						*dpns_p = dpns;
						*ancestor_cell_p = lc;
						return (Node *) nlp->paramval;
					}
				}
			}

			/*
			 * If ancestor is a SubPlan, check the arguments it provides.
			 */
			if (IsA(ancestor, SubPlan))
			{
				SubPlan    *subplan = (SubPlan *) ancestor;
				ListCell   *lc3;
				ListCell   *lc4;

				forboth(lc3, subplan->parParam, lc4, subplan->args)
				{
					int			paramid = lfirst_int(lc3);
					Node	   *arg = (Node *) lfirst(lc4);

					if (paramid == param->paramid)
					{
						/*
						 * Found a match, so return it.  But, since Vars in
						 * the arg are to be evaluated in the surrounding
						 * context, we have to point to the next ancestor item
						 * that is *not* a SubPlan.
						 */
						ListCell   *rest;

						for_each_cell(rest, dpns->ancestors,
									  lnext(dpns->ancestors, lc))
						{
							Node	   *ancestor2 = (Node *) lfirst(rest);

							if (!IsA(ancestor2, SubPlan))
							{
								*dpns_p = dpns;
								*ancestor_cell_p = rest;
								return arg;
							}
						}
						elog(ERROR, "SubPlan cannot be outermost ancestor");
					}
				}

				/* SubPlan isn't a kind of Plan, so skip the rest */
				continue;
			}

			/*
			 * We need not consider the ancestor's initPlan list, since
			 * initplans never have any parParams.
			 */

			/* No luck, crawl up to next ancestor */
			child_plan = (Plan *) ancestor;
		}
	}

	/* No referent found */
	return NULL;
}

/*
 * Display a Param appropriately.
 */
static void
get_parameter(Param *param, deparse_context *context)
{
	Node	   *expr;
	deparse_namespace *dpns;
	ListCell   *ancestor_cell;

	/*
	 * If it's a PARAM_EXEC parameter, try to locate the expression from which
	 * the parameter was computed.  Note that failing to find a referent isn't
	 * an error, since the Param might well be a subplan output rather than an
	 * input.
	 */
	expr = find_param_referent(param, context, &dpns, &ancestor_cell);
	if (expr)
	{
		/* Found a match, so print it */
		deparse_namespace save_dpns;
		bool		save_varprefix;
		bool		need_paren;

		/* Switch attention to the ancestor plan node */
		push_ancestor_plan(dpns, ancestor_cell, &save_dpns);

		/*
		 * Force prefixing of Vars, since they won't belong to the relation
		 * being scanned in the original plan node.
		 */
		save_varprefix = context->varprefix;
		context->varprefix = true;

		/*
		 * A Param's expansion is typically a Var, Aggref, GroupingFunc, or
		 * upper-level Param, which wouldn't need extra parentheses.
		 * Otherwise, insert parens to ensure the expression looks atomic.
		 */
		need_paren = !(IsA(expr, Var) ||
					   IsA(expr, Aggref) ||
					   IsA(expr, GroupingFunc) ||
					   IsA(expr, Param));
		if (need_paren)
			appendStringInfoChar(context->buf, '(');

		get_rule_expr(expr, context, false);

		if (need_paren)
			appendStringInfoChar(context->buf, ')');

		context->varprefix = save_varprefix;

		pop_ancestor_plan(dpns, &save_dpns);

		return;
	}

	/*
	 * If it's an external parameter, see if the outermost namespace provides
	 * function argument names.
	 */
	if (param->paramkind == PARAM_EXTERN && context->namespaces != NIL)
	{
		dpns = llast(context->namespaces);
		if (dpns->argnames &&
			param->paramid > 0 &&
			param->paramid <= dpns->numargs)
		{
			char	   *argname = dpns->argnames[param->paramid - 1];

			if (argname)
			{
				bool		should_qualify = false;
				ListCell   *lc;

				/*
				 * Qualify the parameter name if there are any other deparse
				 * namespaces with range tables.  This avoids qualifying in
				 * trivial cases like "RETURN a + b", but makes it safe in all
				 * other cases.
				 */
				foreach(lc, context->namespaces)
				{
					deparse_namespace *depns = lfirst(lc);

					if (depns->rtable_names != NIL)
					{
						should_qualify = true;
						break;
					}
				}
				if (should_qualify)
				{
					appendStringInfoString(context->buf, quote_identifier(dpns->funcname));
					appendStringInfoChar(context->buf, '.');
				}

				appendStringInfoString(context->buf, quote_identifier(argname));
				return;
			}
		}
	}

	/*
	 * Not PARAM_EXEC, or couldn't find referent: just print $N.
	 */
	appendStringInfo(context->buf, "$%d", param->paramid);
}

/*
 * get_simple_binary_op_name
 *
 * helper function for isSimpleNode
 * will return single char binary operator name, or NULL if it's not
 */
static const char *
get_simple_binary_op_name(OpExpr *expr)
{
	List	   *args = expr->args;

	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);
		const char *op;

		op = generate_operator_name(expr->opno, exprType(arg1), exprType(arg2));
		if (strlen(op) == 1)
			return op;
	}
	return NULL;
}


/*
 * isSimpleNode - check if given node is simple (doesn't need parenthesizing)
 *
 *	true   : simple in the context of parent node's type
 *	false  : not simple
 */
static bool
isSimpleNode(Node *node, Node *parentNode, int prettyFlags)
{
	if (!node)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CoerceToDomainValue:
		case T_SetToDefault:
		case T_CurrentOfExpr:
			/* single words: always simple */
			return true;

		case T_SubscriptingRef:
		case T_ArrayExpr:
		case T_RowExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
		case T_NextValueExpr:
		case T_NullIfExpr:
		case T_Aggref:
		case T_GroupingFunc:
		case T_WindowFunc:
		case T_FuncExpr:
		case T_JsonConstructorExpr:
			/* function-like: name(..) or name[..] */
			return true;

			/* CASE keywords act as parentheses */
		case T_CaseExpr:
			return true;

		case T_FieldSelect:

			/*
			 * appears simple since . has top precedence, unless parent is
			 * T_FieldSelect itself!
			 */
			return !IsA(parentNode, FieldSelect);

		case T_FieldStore:

			/*
			 * treat like FieldSelect (probably doesn't matter)
			 */
			return !IsA(parentNode, FieldStore);

		case T_CoerceToDomain:
			/* maybe simple, check args */
			return isSimpleNode((Node *) ((CoerceToDomain *) node)->arg,
								node, prettyFlags);
		case T_RelabelType:
			return isSimpleNode((Node *) ((RelabelType *) node)->arg,
								node, prettyFlags);
		case T_CoerceViaIO:
			return isSimpleNode((Node *) ((CoerceViaIO *) node)->arg,
								node, prettyFlags);
		case T_ArrayCoerceExpr:
			return isSimpleNode((Node *) ((ArrayCoerceExpr *) node)->arg,
								node, prettyFlags);
		case T_ConvertRowtypeExpr:
			return isSimpleNode((Node *) ((ConvertRowtypeExpr *) node)->arg,
								node, prettyFlags);

		case T_OpExpr:
			{
				/* depends on parent node type; needs further checking */
				if (prettyFlags & PRETTYFLAG_PAREN && IsA(parentNode, OpExpr))
				{
					const char *op;
					const char *parentOp;
					bool		is_lopriop;
					bool		is_hipriop;
					bool		is_lopriparent;
					bool		is_hipriparent;

					op = get_simple_binary_op_name((OpExpr *) node);
					if (!op)
						return false;

					/* We know only the basic operators + - and * / % */
					is_lopriop = (strchr("+-", *op) != NULL);
					is_hipriop = (strchr("*/%", *op) != NULL);
					if (!(is_lopriop || is_hipriop))
						return false;

					parentOp = get_simple_binary_op_name((OpExpr *) parentNode);
					if (!parentOp)
						return false;

					is_lopriparent = (strchr("+-", *parentOp) != NULL);
					is_hipriparent = (strchr("*/%", *parentOp) != NULL);
					if (!(is_lopriparent || is_hipriparent))
						return false;

					if (is_hipriop && is_lopriparent)
						return true;	/* op binds tighter than parent */

					if (is_lopriop && is_hipriparent)
						return false;

					/*
					 * Operators are same priority --- can skip parens only if
					 * we have (a - b) - c, not a - (b - c).
					 */
					if (node == (Node *) linitial(((OpExpr *) parentNode)->args))
						return true;

					return false;
				}
				/* else do the same stuff as for T_SubLink et al. */
			}
			/* FALLTHROUGH */

		case T_SubLink:
		case T_NullTest:
		case T_BooleanTest:
		case T_DistinctExpr:
		case T_JsonIsPredicate:
			switch (nodeTag(parentNode))
			{
				case T_FuncExpr:
					{
						/* special handling for casts and COERCE_SQL_SYNTAX */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST ||
							type == COERCE_SQL_SYNTAX)
							return false;
						return true;	/* own parentheses */
					}
				case T_BoolExpr:	/* lower precedence */
				case T_SubscriptingRef: /* other separators */
				case T_ArrayExpr:	/* other separators */
				case T_RowExpr: /* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:	/* own parentheses */
				case T_XmlExpr: /* own parentheses */
				case T_NullIfExpr:	/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_GroupingFunc:	/* own parentheses */
				case T_WindowFunc:	/* own parentheses */
				case T_CaseExpr:	/* other separators */
					return true;
				default:
					return false;
			}

		case T_BoolExpr:
			switch (nodeTag(parentNode))
			{
				case T_BoolExpr:
					if (prettyFlags & PRETTYFLAG_PAREN)
					{
						BoolExprType type;
						BoolExprType parentType;

						type = ((BoolExpr *) node)->boolop;
						parentType = ((BoolExpr *) parentNode)->boolop;
						switch (type)
						{
							case NOT_EXPR:
							case AND_EXPR:
								if (parentType == AND_EXPR || parentType == OR_EXPR)
									return true;
								break;
							case OR_EXPR:
								if (parentType == OR_EXPR)
									return true;
								break;
						}
					}
					return false;
				case T_FuncExpr:
					{
						/* special handling for casts and COERCE_SQL_SYNTAX */
						CoercionForm type = ((FuncExpr *) parentNode)->funcformat;

						if (type == COERCE_EXPLICIT_CAST ||
							type == COERCE_IMPLICIT_CAST ||
							type == COERCE_SQL_SYNTAX)
							return false;
						return true;	/* own parentheses */
					}
				case T_SubscriptingRef: /* other separators */
				case T_ArrayExpr:	/* other separators */
				case T_RowExpr: /* other separators */
				case T_CoalesceExpr:	/* own parentheses */
				case T_MinMaxExpr:	/* own parentheses */
				case T_XmlExpr: /* own parentheses */
				case T_NullIfExpr:	/* other separators */
				case T_Aggref:	/* own parentheses */
				case T_GroupingFunc:	/* own parentheses */
				case T_WindowFunc:	/* own parentheses */
				case T_CaseExpr:	/* other separators */
					return true;
				default:
					return false;
			}

		case T_JsonValueExpr:
			/* maybe simple, check args */
			return isSimpleNode((Node *) ((JsonValueExpr *) node)->raw_expr,
								node, prettyFlags);

		default:
			break;
	}
	/* those we don't know: in dubio complexo */
	return false;
}


/*
 * appendContextKeyword - append a keyword to buffer
 *
 * If prettyPrint is enabled, perform a line break, and adjust indentation.
 * Otherwise, just append the keyword.
 */
static void
appendContextKeyword(deparse_context *context, const char *str,
					 int indentBefore, int indentAfter, int indentPlus)
{
	StringInfo	buf = context->buf;

	if (PRETTY_INDENT(context))
	{
		int			indentAmount;

		context->indentLevel += indentBefore;

		/* remove any trailing spaces currently in the buffer ... */
		removeStringInfoSpaces(buf);
		/* ... then add a newline and some spaces */
		appendStringInfoChar(buf, '\n');

		if (context->indentLevel < PRETTYINDENT_LIMIT)
			indentAmount = Max(context->indentLevel, 0) + indentPlus;
		else
		{
			/*
			 * If we're indented more than PRETTYINDENT_LIMIT characters, try
			 * to conserve horizontal space by reducing the per-level
			 * indentation.  For best results the scale factor here should
			 * divide all the indent amounts that get added to indentLevel
			 * (PRETTYINDENT_STD, etc).  It's important that the indentation
			 * not grow unboundedly, else deeply-nested trees use O(N^2)
			 * whitespace; so we also wrap modulo PRETTYINDENT_LIMIT.
			 */
			indentAmount = PRETTYINDENT_LIMIT +
				(context->indentLevel - PRETTYINDENT_LIMIT) /
				(PRETTYINDENT_STD / 2);
			indentAmount %= PRETTYINDENT_LIMIT;
			/* scale/wrap logic affects indentLevel, but not indentPlus */
			indentAmount += indentPlus;
		}
		appendStringInfoSpaces(buf, indentAmount);

		appendStringInfoString(buf, str);

		context->indentLevel += indentAfter;
		if (context->indentLevel < 0)
			context->indentLevel = 0;
	}
	else
		appendStringInfoString(buf, str);
}

/*
 * removeStringInfoSpaces - delete trailing spaces from a buffer.
 *
 * Possibly this should move to stringinfo.c at some point.
 */
static void
removeStringInfoSpaces(StringInfo str)
{
	while (str->len > 0 && str->data[str->len - 1] == ' ')
		str->data[--(str->len)] = '\0';
}


/*
 * get_rule_expr_paren	- deparse expr using get_rule_expr,
 * embracing the string with parentheses if necessary for prettyPrint.
 *
 * Never embrace if prettyFlags=0, because it's done in the calling node.
 *
 * Any node that does *not* embrace its argument node by sql syntax (with
 * parentheses, non-operator keywords like CASE/WHEN/ON, or comma etc) should
 * use get_rule_expr_paren instead of get_rule_expr so parentheses can be
 * added.
 */
static void
get_rule_expr_paren(Node *node, deparse_context *context,
					bool showimplicit, Node *parentNode)
{
	bool		need_paren;

	need_paren = PRETTY_PAREN(context) &&
		!isSimpleNode(node, parentNode, context->prettyFlags);

	if (need_paren)
		appendStringInfoChar(context->buf, '(');

	get_rule_expr(node, context, showimplicit);

	if (need_paren)
		appendStringInfoChar(context->buf, ')');
}


/* ----------
 * get_rule_expr			- Parse back an expression
 *
 * Note: showimplicit determines whether we display any implicit cast that
 * is present at the top of the expression tree.  It is a passed argument,
 * not a field of the context struct, because we change the value as we
 * recurse down into the expression.  In general we suppress implicit casts
 * when the result type is known with certainty (eg, the arguments of an
 * OR must be boolean).  We display implicit casts for arguments of functions
 * and operators, since this is needed to be certain that the same function
 * or operator will be chosen when the expression is re-parsed.
 * ----------
 */
static void
get_rule_expr(Node *node, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;

	if (node == NULL)
		return;

	/* Guard against excessively long or deeply-nested queries */
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	/*
	 * Each level of get_rule_expr must emit an indivisible term
	 * (parenthesized if necessary) to ensure result is reparsed into the same
	 * expression tree.  The only exception is that when the input is a List,
	 * we emit the component items comma-separated with no surrounding
	 * decoration; this is convenient for most callers.
	 */
	switch (nodeTag(node))
	{
		case T_Var:
			(void) get_variable((Var *) node, 0, false, context);
			break;

		case T_Const:
			get_const_expr((Const *) node, context, 0);
			break;

		case T_Param:
			get_parameter((Param *) node, context);
			break;

		case T_Aggref:
			get_agg_expr((Aggref *) node, context, (Aggref *) node);
			break;

		case T_GroupingFunc:
			{
				GroupingFunc *gexpr = (GroupingFunc *) node;

				appendStringInfoString(buf, "GROUPING(");
				get_rule_expr((Node *) gexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_WindowFunc:
			get_windowfunc_expr((WindowFunc *) node, context);
			break;

		case T_SubscriptingRef:
			{
				SubscriptingRef *sbsref = (SubscriptingRef *) node;
				bool		need_parens;

				/*
				 * If the argument is a CaseTestExpr, we must be inside a
				 * FieldStore, ie, we are assigning to an element of an array
				 * within a composite column.  Since we already punted on
				 * displaying the FieldStore's target information, just punt
				 * here too, and display only the assignment source
				 * expression.
				 */
				if (IsA(sbsref->refexpr, CaseTestExpr))
				{
					Assert(sbsref->refassgnexpr);
					get_rule_expr((Node *) sbsref->refassgnexpr,
								  context, showimplicit);
					break;
				}

				/*
				 * Parenthesize the argument unless it's a simple Var or a
				 * FieldSelect.  (In particular, if it's another
				 * SubscriptingRef, we *must* parenthesize to avoid
				 * confusion.)
				 */
				need_parens = !IsA(sbsref->refexpr, Var) &&
					!IsA(sbsref->refexpr, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr((Node *) sbsref->refexpr, context, showimplicit);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				/*
				 * If there's a refassgnexpr, we want to print the node in the
				 * format "container[subscripts] := refassgnexpr".  This is
				 * not legal SQL, so decompilation of INSERT or UPDATE
				 * statements should always use processIndirection as part of
				 * the statement-level syntax.  We should only see this when
				 * EXPLAIN tries to print the targetlist of a plan resulting
				 * from such a statement.
				 */
				if (sbsref->refassgnexpr)
				{
					Node	   *refassgnexpr;

					/*
					 * Use processIndirection to print this node's subscripts
					 * as well as any additional field selections or
					 * subscripting in immediate descendants.  It returns the
					 * RHS expr that is actually being "assigned".
					 */
					refassgnexpr = processIndirection(node, context);
					appendStringInfoString(buf, " := ");
					get_rule_expr(refassgnexpr, context, showimplicit);
				}
				else
				{
					/* Just an ordinary container fetch, so print subscripts */
					printSubscripts(sbsref, context);
				}
			}
			break;

		case T_FuncExpr:
			get_func_expr((FuncExpr *) node, context, showimplicit);
			break;

		case T_NamedArgExpr:
			{
				NamedArgExpr *na = (NamedArgExpr *) node;

				appendStringInfo(buf, "%s => ", quote_identifier(na->name));
				get_rule_expr((Node *) na->arg, context, showimplicit);
			}
			break;

		case T_OpExpr:
			get_oper_expr((OpExpr *) node, context);
			break;

		case T_DistinctExpr:
			{
				DistinctExpr *expr = (DistinctExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfoString(buf, " IS DISTINCT FROM ");
				get_rule_expr_paren(arg2, context, true, node);
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullIfExpr:
			{
				NullIfExpr *nullifexpr = (NullIfExpr *) node;

				appendStringInfoString(buf, "NULLIF(");
				get_rule_expr((Node *) nullifexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *expr = (ScalarArrayOpExpr *) node;
				List	   *args = expr->args;
				Node	   *arg1 = (Node *) linitial(args);
				Node	   *arg2 = (Node *) lsecond(args);

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg1, context, true, node);
				appendStringInfo(buf, " %s %s (",
								 generate_operator_name(expr->opno,
														exprType(arg1),
														get_base_element_type(exprType(arg2))),
								 expr->useOr ? "ANY" : "ALL");
				get_rule_expr_paren(arg2, context, true, node);

				/*
				 * There's inherent ambiguity in "x op ANY/ALL (y)" when y is
				 * a bare sub-SELECT.  Since we're here, the sub-SELECT must
				 * be meant as a scalar sub-SELECT yielding an array value to
				 * be used in ScalarArrayOpExpr; but the grammar will
				 * preferentially interpret such a construct as an ANY/ALL
				 * SubLink.  To prevent misparsing the output that way, insert
				 * a dummy coercion (which will be stripped by parse analysis,
				 * so no inefficiency is added in dump and reload).  This is
				 * indeed most likely what the user wrote to get the construct
				 * accepted in the first place.
				 */
				if (IsA(arg2, SubLink) &&
					((SubLink *) arg2)->subLinkType == EXPR_SUBLINK)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(exprType(arg2),
															  exprTypmod(arg2)));
				appendStringInfoChar(buf, ')');
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BoolExpr:
			{
				BoolExpr   *expr = (BoolExpr *) node;
				Node	   *first_arg = linitial(expr->args);
				ListCell   *arg;

				switch (expr->boolop)
				{
					case AND_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						for_each_from(arg, expr->args, 1)
						{
							appendStringInfoString(buf, " AND ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case OR_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						get_rule_expr_paren(first_arg, context,
											false, node);
						for_each_from(arg, expr->args, 1)
						{
							appendStringInfoString(buf, " OR ");
							get_rule_expr_paren((Node *) lfirst(arg), context,
												false, node);
						}
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					case NOT_EXPR:
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, '(');
						appendStringInfoString(buf, "NOT ");
						get_rule_expr_paren(first_arg, context,
											false, node);
						if (!PRETTY_PAREN(context))
							appendStringInfoChar(buf, ')');
						break;

					default:
						elog(ERROR, "unrecognized boolop: %d",
							 (int) expr->boolop);
				}
			}
			break;

		case T_SubLink:
			get_sublink_expr((SubLink *) node, context);
			break;

		case T_SubPlan:
			{
				SubPlan    *subplan = (SubPlan *) node;

				/*
				 * We cannot see an already-planned subplan in rule deparsing,
				 * only while EXPLAINing a query plan.  We don't try to
				 * reconstruct the original SQL, just reference the subplan
				 * that appears elsewhere in EXPLAIN's result.
				 */
				if (subplan->useHashTable)
					appendStringInfo(buf, "(hashed %s)", subplan->plan_name);
				else
					appendStringInfo(buf, "(%s)", subplan->plan_name);
			}
			break;

		case T_AlternativeSubPlan:
			{
				AlternativeSubPlan *asplan = (AlternativeSubPlan *) node;
				ListCell   *lc;

				/*
				 * This case cannot be reached in normal usage, since no
				 * AlternativeSubPlan can appear either in parsetrees or
				 * finished plan trees.  We keep it just in case somebody
				 * wants to use this code to print planner data structures.
				 */
				appendStringInfoString(buf, "(alternatives: ");
				foreach(lc, asplan->subplans)
				{
					SubPlan    *splan = lfirst_node(SubPlan, lc);

					if (splan->useHashTable)
						appendStringInfo(buf, "hashed %s", splan->plan_name);
					else
						appendStringInfoString(buf, splan->plan_name);
					if (lnext(asplan->subplans, lc))
						appendStringInfoString(buf, " or ");
				}
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_FieldSelect:
			{
				FieldSelect *fselect = (FieldSelect *) node;
				Node	   *arg = (Node *) fselect->arg;
				int			fno = fselect->fieldnum;
				const char *fieldname;
				bool		need_parens;

				/*
				 * Parenthesize the argument unless it's an SubscriptingRef or
				 * another FieldSelect.  Note in particular that it would be
				 * WRONG to not parenthesize a Var argument; simplicity is not
				 * the issue here, having the right number of names is.
				 */
				need_parens = !IsA(arg, SubscriptingRef) &&
					!IsA(arg, FieldSelect);
				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr(arg, context, true);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				/*
				 * Get and print the field name.
				 */
				fieldname = get_name_for_var_field((Var *) arg, fno,
												   0, context);
				appendStringInfo(buf, ".%s", quote_identifier(fieldname));
			}
			break;

		case T_FieldStore:
			{
				FieldStore *fstore = (FieldStore *) node;
				bool		need_parens;

				/*
				 * There is no good way to represent a FieldStore as real SQL,
				 * so decompilation of INSERT or UPDATE statements should
				 * always use processIndirection as part of the
				 * statement-level syntax.  We should only get here when
				 * EXPLAIN tries to print the targetlist of a plan resulting
				 * from such a statement.  The plan case is even harder than
				 * ordinary rules would be, because the planner tries to
				 * collapse multiple assignments to the same field or subfield
				 * into one FieldStore; so we can see a list of target fields
				 * not just one, and the arguments could be FieldStores
				 * themselves.  We don't bother to try to print the target
				 * field names; we just print the source arguments, with a
				 * ROW() around them if there's more than one.  This isn't
				 * terribly complete, but it's probably good enough for
				 * EXPLAIN's purposes; especially since anything more would be
				 * either hopelessly confusing or an even poorer
				 * representation of what the plan is actually doing.
				 */
				need_parens = (list_length(fstore->newvals) != 1);
				if (need_parens)
					appendStringInfoString(buf, "ROW(");
				get_rule_expr((Node *) fstore->newvals, context, showimplicit);
				if (need_parens)
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_RelabelType:
			{
				RelabelType *relabel = (RelabelType *) node;
				Node	   *arg = (Node *) relabel->arg;

				if (relabel->relabelformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  relabel->resulttype,
									  relabel->resulttypmod,
									  node);
				}
			}
			break;

		case T_CoerceViaIO:
			{
				CoerceViaIO *iocoerce = (CoerceViaIO *) node;
				Node	   *arg = (Node *) iocoerce->arg;

				if (iocoerce->coerceformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  iocoerce->resulttype,
									  -1,
									  node);
				}
			}
			break;

		case T_ArrayCoerceExpr:
			{
				ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;
				Node	   *arg = (Node *) acoerce->arg;

				if (acoerce->coerceformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  acoerce->resulttype,
									  acoerce->resulttypmod,
									  node);
				}
			}
			break;

		case T_ConvertRowtypeExpr:
			{
				ConvertRowtypeExpr *convert = (ConvertRowtypeExpr *) node;
				Node	   *arg = (Node *) convert->arg;

				if (convert->convertformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr_paren(arg, context, false, node);
				}
				else
				{
					get_coercion_expr(arg, context,
									  convert->resulttype, -1,
									  node);
				}
			}
			break;

		case T_CollateExpr:
			{
				CollateExpr *collate = (CollateExpr *) node;
				Node	   *arg = (Node *) collate->arg;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren(arg, context, showimplicit, node);
				appendStringInfo(buf, " COLLATE %s",
								 generate_collation_name(collate->collOid));
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_CaseExpr:
			{
				CaseExpr   *caseexpr = (CaseExpr *) node;
				ListCell   *temp;

				appendContextKeyword(context, "CASE",
									 0, PRETTYINDENT_VAR, 0);
				if (caseexpr->arg)
				{
					appendStringInfoChar(buf, ' ');
					get_rule_expr((Node *) caseexpr->arg, context, true);
				}
				foreach(temp, caseexpr->args)
				{
					CaseWhen   *when = (CaseWhen *) lfirst(temp);
					Node	   *w = (Node *) when->expr;

					if (caseexpr->arg)
					{
						/*
						 * The parser should have produced WHEN clauses of the
						 * form "CaseTestExpr = RHS", possibly with an
						 * implicit coercion inserted above the CaseTestExpr.
						 * For accurate decompilation of rules it's essential
						 * that we show just the RHS.  However in an
						 * expression that's been through the optimizer, the
						 * WHEN clause could be almost anything (since the
						 * equality operator could have been expanded into an
						 * inline function).  If we don't recognize the form
						 * of the WHEN clause, just punt and display it as-is.
						 */
						if (IsA(w, OpExpr))
						{
							List	   *args = ((OpExpr *) w)->args;

							if (list_length(args) == 2 &&
								IsA(strip_implicit_coercions(linitial(args)),
									CaseTestExpr))
								w = (Node *) lsecond(args);
						}
					}

					if (!PRETTY_INDENT(context))
						appendStringInfoChar(buf, ' ');
					appendContextKeyword(context, "WHEN ",
										 0, 0, 0);
					get_rule_expr(w, context, false);
					appendStringInfoString(buf, " THEN ");
					get_rule_expr((Node *) when->result, context, true);
				}
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "ELSE ",
									 0, 0, 0);
				get_rule_expr((Node *) caseexpr->defresult, context, true);
				if (!PRETTY_INDENT(context))
					appendStringInfoChar(buf, ' ');
				appendContextKeyword(context, "END",
									 -PRETTYINDENT_VAR, 0, 0);
			}
			break;

		case T_CaseTestExpr:
			{
				/*
				 * Normally we should never get here, since for expressions
				 * that can contain this node type we attempt to avoid
				 * recursing to it.  But in an optimized expression we might
				 * be unable to avoid that (see comments for CaseExpr).  If we
				 * do see one, print it as CASE_TEST_EXPR.
				 */
				appendStringInfoString(buf, "CASE_TEST_EXPR");
			}
			break;

		case T_ArrayExpr:
			{
				ArrayExpr  *arrayexpr = (ArrayExpr *) node;

				appendStringInfoString(buf, "ARRAY[");
				get_rule_expr((Node *) arrayexpr->elements, context, true);
				appendStringInfoChar(buf, ']');

				/*
				 * If the array isn't empty, we assume its elements are
				 * coerced to the desired type.  If it's empty, though, we
				 * need an explicit coercion to the array type.
				 */
				if (arrayexpr->elements == NIL)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(arrayexpr->array_typeid, -1));
			}
			break;

		case T_RowExpr:
			{
				RowExpr    *rowexpr = (RowExpr *) node;
				TupleDesc	tupdesc = NULL;
				ListCell   *arg;
				int			i;
				char	   *sep;

				/*
				 * If it's a named type and not RECORD, we may have to skip
				 * dropped columns and/or claim there are NULLs for added
				 * columns.
				 */
				if (rowexpr->row_typeid != RECORDOID)
				{
					tupdesc = lookup_rowtype_tupdesc(rowexpr->row_typeid, -1);
					Assert(list_length(rowexpr->args) <= tupdesc->natts);
				}

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.
				 */
				appendStringInfoString(buf, "ROW(");
				sep = "";
				i = 0;
				foreach(arg, rowexpr->args)
				{
					Node	   *e = (Node *) lfirst(arg);

					if (tupdesc == NULL ||
						!TupleDescAttr(tupdesc, i)->attisdropped)
					{
						appendStringInfoString(buf, sep);
						/* Whole-row Vars need special treatment here */
						get_rule_expr_toplevel(e, context, true);
						sep = ", ";
					}
					i++;
				}
				if (tupdesc != NULL)
				{
					while (i < tupdesc->natts)
					{
						if (!TupleDescAttr(tupdesc, i)->attisdropped)
						{
							appendStringInfoString(buf, sep);
							appendStringInfoString(buf, "NULL");
							sep = ", ";
						}
						i++;
					}

					ReleaseTupleDesc(tupdesc);
				}
				appendStringInfoChar(buf, ')');
				if (rowexpr->row_format == COERCE_EXPLICIT_CAST)
					appendStringInfo(buf, "::%s",
									 format_type_with_typemod(rowexpr->row_typeid, -1));
			}
			break;

		case T_RowCompareExpr:
			{
				RowCompareExpr *rcexpr = (RowCompareExpr *) node;

				/*
				 * SQL99 allows "ROW" to be omitted when there is more than
				 * one column, but for simplicity we always print it.  Within
				 * a ROW expression, whole-row Vars need special treatment, so
				 * use get_rule_list_toplevel.
				 */
				appendStringInfoString(buf, "(ROW(");
				get_rule_list_toplevel(rcexpr->largs, context, true);

				/*
				 * We assume that the name of the first-column operator will
				 * do for all the rest too.  This is definitely open to
				 * failure, eg if some but not all operators were renamed
				 * since the construct was parsed, but there seems no way to
				 * be perfect.
				 */
				appendStringInfo(buf, ") %s ROW(",
								 generate_operator_name(linitial_oid(rcexpr->opnos),
														exprType(linitial(rcexpr->largs)),
														exprType(linitial(rcexpr->rargs))));
				get_rule_list_toplevel(rcexpr->rargs, context, true);
				appendStringInfoString(buf, "))");
			}
			break;

		case T_CoalesceExpr:
			{
				CoalesceExpr *coalesceexpr = (CoalesceExpr *) node;

				appendStringInfoString(buf, "COALESCE(");
				get_rule_expr((Node *) coalesceexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_MinMaxExpr:
			{
				MinMaxExpr *minmaxexpr = (MinMaxExpr *) node;

				switch (minmaxexpr->op)
				{
					case IS_GREATEST:
						appendStringInfoString(buf, "GREATEST(");
						break;
					case IS_LEAST:
						appendStringInfoString(buf, "LEAST(");
						break;
				}
				get_rule_expr((Node *) minmaxexpr->args, context, true);
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_SQLValueFunction:
			{
				SQLValueFunction *svf = (SQLValueFunction *) node;

				/*
				 * Note: this code knows that typmod for time, timestamp, and
				 * timestamptz just prints as integer.
				 */
				switch (svf->op)
				{
					case SVFOP_CURRENT_DATE:
						appendStringInfoString(buf, "CURRENT_DATE");
						break;
					case SVFOP_CURRENT_TIME:
						appendStringInfoString(buf, "CURRENT_TIME");
						break;
					case SVFOP_CURRENT_TIME_N:
						appendStringInfo(buf, "CURRENT_TIME(%d)", svf->typmod);
						break;
					case SVFOP_CURRENT_TIMESTAMP:
						appendStringInfoString(buf, "CURRENT_TIMESTAMP");
						break;
					case SVFOP_CURRENT_TIMESTAMP_N:
						appendStringInfo(buf, "CURRENT_TIMESTAMP(%d)",
										 svf->typmod);
						break;
					case SVFOP_LOCALTIME:
						appendStringInfoString(buf, "LOCALTIME");
						break;
					case SVFOP_LOCALTIME_N:
						appendStringInfo(buf, "LOCALTIME(%d)", svf->typmod);
						break;
					case SVFOP_LOCALTIMESTAMP:
						appendStringInfoString(buf, "LOCALTIMESTAMP");
						break;
					case SVFOP_LOCALTIMESTAMP_N:
						appendStringInfo(buf, "LOCALTIMESTAMP(%d)",
										 svf->typmod);
						break;
					case SVFOP_CURRENT_ROLE:
						appendStringInfoString(buf, "CURRENT_ROLE");
						break;
					case SVFOP_CURRENT_USER:
						appendStringInfoString(buf, "CURRENT_USER");
						break;
					case SVFOP_USER:
						appendStringInfoString(buf, "USER");
						break;
					case SVFOP_SESSION_USER:
						appendStringInfoString(buf, "SESSION_USER");
						break;
					case SVFOP_CURRENT_CATALOG:
						appendStringInfoString(buf, "CURRENT_CATALOG");
						break;
					case SVFOP_CURRENT_SCHEMA:
						appendStringInfoString(buf, "CURRENT_SCHEMA");
						break;
				}
			}
			break;

		case T_XmlExpr:
			{
				XmlExpr    *xexpr = (XmlExpr *) node;
				bool		needcomma = false;
				ListCell   *arg;
				ListCell   *narg;
				Const	   *con;

				switch (xexpr->op)
				{
					case IS_XMLCONCAT:
						appendStringInfoString(buf, "XMLCONCAT(");
						break;
					case IS_XMLELEMENT:
						appendStringInfoString(buf, "XMLELEMENT(");
						break;
					case IS_XMLFOREST:
						appendStringInfoString(buf, "XMLFOREST(");
						break;
					case IS_XMLPARSE:
						appendStringInfoString(buf, "XMLPARSE(");
						break;
					case IS_XMLPI:
						appendStringInfoString(buf, "XMLPI(");
						break;
					case IS_XMLROOT:
						appendStringInfoString(buf, "XMLROOT(");
						break;
					case IS_XMLSERIALIZE:
						appendStringInfoString(buf, "XMLSERIALIZE(");
						break;
					case IS_DOCUMENT:
						break;
				}
				if (xexpr->op == IS_XMLPARSE || xexpr->op == IS_XMLSERIALIZE)
				{
					if (xexpr->xmloption == XMLOPTION_DOCUMENT)
						appendStringInfoString(buf, "DOCUMENT ");
					else
						appendStringInfoString(buf, "CONTENT ");
				}
				if (xexpr->name)
				{
					appendStringInfo(buf, "NAME %s",
									 quote_identifier(map_xml_name_to_sql_identifier(xexpr->name)));
					needcomma = true;
				}
				if (xexpr->named_args)
				{
					if (xexpr->op != IS_XMLFOREST)
					{
						if (needcomma)
							appendStringInfoString(buf, ", ");
						appendStringInfoString(buf, "XMLATTRIBUTES(");
						needcomma = false;
					}
					forboth(arg, xexpr->named_args, narg, xexpr->arg_names)
					{
						Node	   *e = (Node *) lfirst(arg);
						char	   *argname = strVal(lfirst(narg));

						if (needcomma)
							appendStringInfoString(buf, ", ");
						get_rule_expr((Node *) e, context, true);
						appendStringInfo(buf, " AS %s",
										 quote_identifier(map_xml_name_to_sql_identifier(argname)));
						needcomma = true;
					}
					if (xexpr->op != IS_XMLFOREST)
						appendStringInfoChar(buf, ')');
				}
				if (xexpr->args)
				{
					if (needcomma)
						appendStringInfoString(buf, ", ");
					switch (xexpr->op)
					{
						case IS_XMLCONCAT:
						case IS_XMLELEMENT:
						case IS_XMLFOREST:
						case IS_XMLPI:
						case IS_XMLSERIALIZE:
							/* no extra decoration needed */
							get_rule_expr((Node *) xexpr->args, context, true);
							break;
						case IS_XMLPARSE:
							Assert(list_length(xexpr->args) == 2);

							get_rule_expr((Node *) linitial(xexpr->args),
										  context, true);

							con = lsecond_node(Const, xexpr->args);
							Assert(!con->constisnull);
							if (DatumGetBool(con->constvalue))
								appendStringInfoString(buf,
													   " PRESERVE WHITESPACE");
							else
								appendStringInfoString(buf,
													   " STRIP WHITESPACE");
							break;
						case IS_XMLROOT:
							Assert(list_length(xexpr->args) == 3);

							get_rule_expr((Node *) linitial(xexpr->args),
										  context, true);

							appendStringInfoString(buf, ", VERSION ");
							con = (Const *) lsecond(xexpr->args);
							if (IsA(con, Const) &&
								con->constisnull)
								appendStringInfoString(buf, "NO VALUE");
							else
								get_rule_expr((Node *) con, context, false);

							con = lthird_node(Const, xexpr->args);
							if (con->constisnull)
								 /* suppress STANDALONE NO VALUE */ ;
							else
							{
								switch (DatumGetInt32(con->constvalue))
								{
									case XML_STANDALONE_YES:
										appendStringInfoString(buf,
															   ", STANDALONE YES");
										break;
									case XML_STANDALONE_NO:
										appendStringInfoString(buf,
															   ", STANDALONE NO");
										break;
									case XML_STANDALONE_NO_VALUE:
										appendStringInfoString(buf,
															   ", STANDALONE NO VALUE");
										break;
									default:
										break;
								}
							}
							break;
						case IS_DOCUMENT:
							get_rule_expr_paren((Node *) xexpr->args, context, false, node);
							break;
					}
				}
				if (xexpr->op == IS_XMLSERIALIZE)
				{
					appendStringInfo(buf, " AS %s",
									 format_type_with_typemod(xexpr->type,
															  xexpr->typmod));
					if (xexpr->indent)
						appendStringInfoString(buf, " INDENT");
					else
						appendStringInfoString(buf, " NO INDENT");
				}

				if (xexpr->op == IS_DOCUMENT)
					appendStringInfoString(buf, " IS DOCUMENT");
				else
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_NullTest:
			{
				NullTest   *ntest = (NullTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) ntest->arg, context, true, node);

				/*
				 * For scalar inputs, we prefer to print as IS [NOT] NULL,
				 * which is shorter and traditional.  If it's a rowtype input
				 * but we're applying a scalar test, must print IS [NOT]
				 * DISTINCT FROM NULL to be semantically correct.
				 */
				if (ntest->argisrow ||
					!type_is_rowtype(exprType((Node *) ntest->arg)))
				{
					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							appendStringInfoString(buf, " IS NULL");
							break;
						case IS_NOT_NULL:
							appendStringInfoString(buf, " IS NOT NULL");
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
					}
				}
				else
				{
					switch (ntest->nulltesttype)
					{
						case IS_NULL:
							appendStringInfoString(buf, " IS NOT DISTINCT FROM NULL");
							break;
						case IS_NOT_NULL:
							appendStringInfoString(buf, " IS DISTINCT FROM NULL");
							break;
						default:
							elog(ERROR, "unrecognized nulltesttype: %d",
								 (int) ntest->nulltesttype);
					}
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_BooleanTest:
			{
				BooleanTest *btest = (BooleanTest *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, '(');
				get_rule_expr_paren((Node *) btest->arg, context, false, node);
				switch (btest->booltesttype)
				{
					case IS_TRUE:
						appendStringInfoString(buf, " IS TRUE");
						break;
					case IS_NOT_TRUE:
						appendStringInfoString(buf, " IS NOT TRUE");
						break;
					case IS_FALSE:
						appendStringInfoString(buf, " IS FALSE");
						break;
					case IS_NOT_FALSE:
						appendStringInfoString(buf, " IS NOT FALSE");
						break;
					case IS_UNKNOWN:
						appendStringInfoString(buf, " IS UNKNOWN");
						break;
					case IS_NOT_UNKNOWN:
						appendStringInfoString(buf, " IS NOT UNKNOWN");
						break;
					default:
						elog(ERROR, "unrecognized booltesttype: %d",
							 (int) btest->booltesttype);
				}
				if (!PRETTY_PAREN(context))
					appendStringInfoChar(buf, ')');
			}
			break;

		case T_CoerceToDomain:
			{
				CoerceToDomain *ctest = (CoerceToDomain *) node;
				Node	   *arg = (Node *) ctest->arg;

				if (ctest->coercionformat == COERCE_IMPLICIT_CAST &&
					!showimplicit)
				{
					/* don't show the implicit cast */
					get_rule_expr(arg, context, false);
				}
				else
				{
					get_coercion_expr(arg, context,
									  ctest->resulttype,
									  ctest->resulttypmod,
									  node);
				}
			}
			break;

		case T_CoerceToDomainValue:
			appendStringInfoString(buf, "VALUE");
			break;

		case T_SetToDefault:
			appendStringInfoString(buf, "DEFAULT");
			break;

		case T_CurrentOfExpr:
			{
				CurrentOfExpr *cexpr = (CurrentOfExpr *) node;

				if (cexpr->cursor_name)
					appendStringInfo(buf, "CURRENT OF %s",
									 quote_identifier(cexpr->cursor_name));
				else
					appendStringInfo(buf, "CURRENT OF $%d",
									 cexpr->cursor_param);
			}
			break;

		case T_NextValueExpr:
			{
				NextValueExpr *nvexpr = (NextValueExpr *) node;

				/*
				 * This isn't exactly nextval(), but that seems close enough
				 * for EXPLAIN's purposes.
				 */
				appendStringInfoString(buf, "nextval(");
				simple_quote_literal(buf,
									 generate_relation_name(nvexpr->seqid,
															NIL));
				appendStringInfoChar(buf, ')');
			}
			break;

		case T_InferenceElem:
			{
				InferenceElem *iexpr = (InferenceElem *) node;
				bool		save_varprefix;
				bool		need_parens;

				/*
				 * InferenceElem can only refer to target relation, so a
				 * prefix is not useful, and indeed would cause parse errors.
				 */
				save_varprefix = context->varprefix;
				context->varprefix = false;

				/*
				 * Parenthesize the element unless it's a simple Var or a bare
				 * function call.  Follows pg_get_indexdef_worker().
				 */
				need_parens = !IsA(iexpr->expr, Var);
				if (IsA(iexpr->expr, FuncExpr) &&
					((FuncExpr *) iexpr->expr)->funcformat ==
					COERCE_EXPLICIT_CALL)
					need_parens = false;

				if (need_parens)
					appendStringInfoChar(buf, '(');
				get_rule_expr((Node *) iexpr->expr,
							  context, false);
				if (need_parens)
					appendStringInfoChar(buf, ')');

				context->varprefix = save_varprefix;

				if (iexpr->infercollid)
					appendStringInfo(buf, " COLLATE %s",
									 generate_collation_name(iexpr->infercollid));

				/* Add the operator class name, if not default */
				if (iexpr->inferopclass)
				{
					Oid			inferopclass = iexpr->inferopclass;
					Oid			inferopcinputtype = get_opclass_input_type(iexpr->inferopclass);

					get_opclass_name(inferopclass, inferopcinputtype, buf);
				}
			}
			break;

		case T_PartitionBoundSpec:
			{
				PartitionBoundSpec *spec = (PartitionBoundSpec *) node;
				ListCell   *cell;
				char	   *sep;

				if (spec->is_default)
				{
					appendStringInfoString(buf, "DEFAULT");
					break;
				}

				switch (spec->strategy)
				{
					case PARTITION_STRATEGY_HASH:
						Assert(spec->modulus > 0 && spec->remainder >= 0);
						Assert(spec->modulus > spec->remainder);

						appendStringInfoString(buf, "FOR VALUES");
						appendStringInfo(buf, " WITH (modulus %d, remainder %d)",
										 spec->modulus, spec->remainder);
						break;

					case PARTITION_STRATEGY_LIST:
						Assert(spec->listdatums != NIL);

						appendStringInfoString(buf, "FOR VALUES IN (");
						sep = "";
						foreach(cell, spec->listdatums)
						{
							Const	   *val = lfirst_node(Const, cell);

							appendStringInfoString(buf, sep);
							get_const_expr(val, context, -1);
							sep = ", ";
						}

						appendStringInfoChar(buf, ')');
						break;

					case PARTITION_STRATEGY_RANGE:
						Assert(spec->lowerdatums != NIL &&
							   spec->upperdatums != NIL &&
							   list_length(spec->lowerdatums) ==
							   list_length(spec->upperdatums));

						appendStringInfo(buf, "FOR VALUES FROM %s TO %s",
										 get_range_partbound_string(spec->lowerdatums),
										 get_range_partbound_string(spec->upperdatums));
						break;

					default:
						elog(ERROR, "unrecognized partition strategy: %d",
							 (int) spec->strategy);
						break;
				}
			}
			break;

		case T_JsonValueExpr:
			{
				JsonValueExpr *jve = (JsonValueExpr *) node;

				get_rule_expr((Node *) jve->raw_expr, context, false);
				get_json_format(jve->format, context->buf);
			}
			break;

		case T_JsonConstructorExpr:
			get_json_constructor((JsonConstructorExpr *) node, context, false);
			break;

		case T_JsonIsPredicate:
			{
				JsonIsPredicate *pred = (JsonIsPredicate *) node;

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(context->buf, '(');

				get_rule_expr_paren(pred->expr, context, true, node);

				appendStringInfoString(context->buf, " IS JSON");

				/* TODO: handle FORMAT clause */

				switch (pred->item_type)
				{
					case JS_TYPE_SCALAR:
						appendStringInfoString(context->buf, " SCALAR");
						break;
					case JS_TYPE_ARRAY:
						appendStringInfoString(context->buf, " ARRAY");
						break;
					case JS_TYPE_OBJECT:
						appendStringInfoString(context->buf, " OBJECT");
						break;
					default:
						break;
				}

				if (pred->unique_keys)
					appendStringInfoString(context->buf, " WITH UNIQUE KEYS");

				if (!PRETTY_PAREN(context))
					appendStringInfoChar(context->buf, ')');
			}
			break;

		case T_List:
			{
				char	   *sep;
				ListCell   *l;

				sep = "";
				foreach(l, (List *) node)
				{
					appendStringInfoString(buf, sep);
					get_rule_expr((Node *) lfirst(l), context, showimplicit);
					sep = ", ";
				}
			}
			break;

		case T_TableFunc:
			get_tablefunc((TableFunc *) node, context, showimplicit);
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(node));
			break;
	}
}

/*
 * get_rule_expr_toplevel		- Parse back a toplevel expression
 *
 * Same as get_rule_expr(), except that if the expr is just a Var, we pass
 * istoplevel = true not false to get_variable().  This causes whole-row Vars
 * to get printed with decoration that will prevent expansion of "*".
 * We need to use this in contexts such as ROW() and VALUES(), where the
 * parser would expand "foo.*" appearing at top level.  (In principle we'd
 * use this in get_target_list() too, but that has additional worries about
 * whether to print AS, so it needs to invoke get_variable() directly anyway.)
 */
static void
get_rule_expr_toplevel(Node *node, deparse_context *context,
					   bool showimplicit)
{
	if (node && IsA(node, Var))
		(void) get_variable((Var *) node, 0, true, context);
	else
		get_rule_expr(node, context, showimplicit);
}

/*
 * get_rule_list_toplevel		- Parse back a list of toplevel expressions
 *
 * Apply get_rule_expr_toplevel() to each element of a List.
 *
 * This adds commas between the expressions, but caller is responsible
 * for printing surrounding decoration.
 */
static void
get_rule_list_toplevel(List *lst, deparse_context *context,
					   bool showimplicit)
{
	const char *sep;
	ListCell   *lc;

	sep = "";
	foreach(lc, lst)
	{
		Node	   *e = (Node *) lfirst(lc);

		appendStringInfoString(context->buf, sep);
		get_rule_expr_toplevel(e, context, showimplicit);
		sep = ", ";
	}
}

/*
 * get_rule_expr_funccall		- Parse back a function-call expression
 *
 * Same as get_rule_expr(), except that we guarantee that the output will
 * look like a function call, or like one of the things the grammar treats as
 * equivalent to a function call (see the func_expr_windowless production).
 * This is needed in places where the grammar uses func_expr_windowless and
 * you can't substitute a parenthesized a_expr.  If what we have isn't going
 * to look like a function call, wrap it in a dummy CAST() expression, which
 * will satisfy the grammar --- and, indeed, is likely what the user wrote to
 * produce such a thing.
 */
static void
get_rule_expr_funccall(Node *node, deparse_context *context,
					   bool showimplicit)
{
	if (looks_like_function(node))
		get_rule_expr(node, context, showimplicit);
	else
	{
		StringInfo	buf = context->buf;

		appendStringInfoString(buf, "CAST(");
		/* no point in showing any top-level implicit cast */
		get_rule_expr(node, context, false);
		appendStringInfo(buf, " AS %s)",
						 format_type_with_typemod(exprType(node),
												  exprTypmod(node)));
	}
}

/*
 * Helper function to identify node types that satisfy func_expr_windowless.
 * If in doubt, "false" is always a safe answer.
 */
static bool
looks_like_function(Node *node)
{
	if (node == NULL)
		return false;			/* probably shouldn't happen */
	switch (nodeTag(node))
	{
		case T_FuncExpr:
			/* OK, unless it's going to deparse as a cast */
			return (((FuncExpr *) node)->funcformat == COERCE_EXPLICIT_CALL ||
					((FuncExpr *) node)->funcformat == COERCE_SQL_SYNTAX);
		case T_NullIfExpr:
		case T_CoalesceExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
			/* these are all accepted by func_expr_common_subexpr */
			return true;
		default:
			break;
	}
	return false;
}


/*
 * get_oper_expr			- Parse back an OpExpr node
 */
static void
get_oper_expr(OpExpr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			opno = expr->opno;
	List	   *args = expr->args;

	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, '(');
	if (list_length(args) == 2)
	{
		/* binary operator */
		Node	   *arg1 = (Node *) linitial(args);
		Node	   *arg2 = (Node *) lsecond(args);

		get_rule_expr_paren(arg1, context, true, (Node *) expr);
		appendStringInfo(buf, " %s ",
						 generate_operator_name(opno,
												exprType(arg1),
												exprType(arg2)));
		get_rule_expr_paren(arg2, context, true, (Node *) expr);
	}
	else
	{
		/* prefix operator */
		Node	   *arg = (Node *) linitial(args);

		appendStringInfo(buf, "%s ",
						 generate_operator_name(opno,
												InvalidOid,
												exprType(arg)));
		get_rule_expr_paren(arg, context, true, (Node *) expr);
	}
	if (!PRETTY_PAREN(context))
		appendStringInfoChar(buf, ')');
}

/*
 * get_func_expr			- Parse back a FuncExpr node
 */
static void
get_func_expr(FuncExpr *expr, deparse_context *context,
			  bool showimplicit)
{
	StringInfo	buf = context->buf;
	Oid			funcoid = expr->funcid;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	List	   *argnames;
	bool		use_variadic;
	ListCell   *l;

	/*
	 * If the function call came from an implicit coercion, then just show the
	 * first argument --- unless caller wants to see implicit coercions.
	 */
	if (expr->funcformat == COERCE_IMPLICIT_CAST && !showimplicit)
	{
		get_rule_expr_paren((Node *) linitial(expr->args), context,
							false, (Node *) expr);
		return;
	}

	/*
	 * If the function call came from a cast, then show the first argument
	 * plus an explicit cast operation.
	 */
	if (expr->funcformat == COERCE_EXPLICIT_CAST ||
		expr->funcformat == COERCE_IMPLICIT_CAST)
	{
		Node	   *arg = linitial(expr->args);
		Oid			rettype = expr->funcresulttype;
		int32		coercedTypmod;

		/* Get the typmod if this is a length-coercion function */
		(void) exprIsLengthCoercion((Node *) expr, &coercedTypmod);

		get_coercion_expr(arg, context,
						  rettype, coercedTypmod,
						  (Node *) expr);

		return;
	}

	/*
	 * If the function was called using one of the SQL spec's random special
	 * syntaxes, try to reproduce that.  If we don't recognize the function,
	 * fall through.
	 */
	if (expr->funcformat == COERCE_SQL_SYNTAX)
	{
		if (get_func_sql_syntax(expr, context))
			return;
	}

	/*
	 * Normal function: display as proname(args).  First we need to extract
	 * the argument datatypes.
	 */
	if (list_length(expr->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments")));
	nargs = 0;
	argnames = NIL;
	foreach(l, expr->args)
	{
		Node	   *arg = (Node *) lfirst(l);

		if (IsA(arg, NamedArgExpr))
			argnames = lappend(argnames, ((NamedArgExpr *) arg)->name);
		argtypes[nargs] = exprType(arg);
		nargs++;
	}

	appendStringInfo(buf, "%s(",
					 generate_function_name(funcoid, nargs,
											argnames, argtypes,
											expr->funcvariadic,
											&use_variadic,
											context->inGroupBy));
	nargs = 0;
	foreach(l, expr->args)
	{
		if (nargs++ > 0)
			appendStringInfoString(buf, ", ");
		if (use_variadic && lnext(expr->args, l) == NULL)
			appendStringInfoString(buf, "VARIADIC ");
		get_rule_expr((Node *) lfirst(l), context, true);
	}
	appendStringInfoChar(buf, ')');
}

/*
 * get_agg_expr			- Parse back an Aggref node
 */
static void
get_agg_expr(Aggref *aggref, deparse_context *context,
			 Aggref *original_aggref)
{
	get_agg_expr_helper(aggref, context, original_aggref, NULL, NULL,
						false);
}

/*
 * get_agg_expr_helper		- subroutine for get_agg_expr and
 *							get_json_agg_constructor
 */
static void
get_agg_expr_helper(Aggref *aggref, deparse_context *context,
					Aggref *original_aggref, const char *funcname,
					const char *options, bool is_json_objectagg)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	bool		use_variadic = false;

	/*
	 * For a combining aggregate, we look up and deparse the corresponding
	 * partial aggregate instead.  This is necessary because our input
	 * argument list has been replaced; the new argument list always has just
	 * one element, which will point to a partial Aggref that supplies us with
	 * transition states to combine.
	 */
	if (DO_AGGSPLIT_COMBINE(aggref->aggsplit))
	{
		TargetEntry *tle;

		Assert(list_length(aggref->args) == 1);
		tle = linitial_node(TargetEntry, aggref->args);
		resolve_special_varno((Node *) tle->expr, context,
							  get_agg_combine_expr, original_aggref);
		return;
	}

	/*
	 * Mark as PARTIAL, if appropriate.  We look to the original aggref so as
	 * to avoid printing this when recursing from the code just above.
	 */
	if (DO_AGGSPLIT_SKIPFINAL(original_aggref->aggsplit))
		appendStringInfoString(buf, "PARTIAL ");

	/* Extract the argument types as seen by the parser */
	nargs = get_aggregate_argtypes(aggref, argtypes);

	if (!funcname)
		funcname = generate_function_name(aggref->aggfnoid, nargs, NIL,
										  argtypes, aggref->aggvariadic,
										  &use_variadic,
										  context->inGroupBy);

	/* Print the aggregate name, schema-qualified if needed */
	appendStringInfo(buf, "%s(%s", funcname,
					 (aggref->aggdistinct != NIL) ? "DISTINCT " : "");

	if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
	{
		/*
		 * Ordered-set aggregates do not use "*" syntax.  Also, we needn't
		 * worry about inserting VARIADIC.  So we can just dump the direct
		 * args as-is.
		 */
		Assert(!aggref->aggvariadic);
		get_rule_expr((Node *) aggref->aggdirectargs, context, true);
		Assert(aggref->aggorder != NIL);
		appendStringInfoString(buf, ") WITHIN GROUP (ORDER BY ");
		get_rule_orderby(aggref->aggorder, aggref->args, false, context);
	}
	else
	{
		/* aggstar can be set only in zero-argument aggregates */
		if (aggref->aggstar)
			appendStringInfoChar(buf, '*');
		else
		{
			ListCell   *l;
			int			i;

			i = 0;
			foreach(l, aggref->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(l);
				Node	   *arg = (Node *) tle->expr;

				Assert(!IsA(arg, NamedArgExpr));
				if (tle->resjunk)
					continue;
				if (i++ > 0)
				{
					if (is_json_objectagg)
					{
						/*
						 * the ABSENT ON NULL and WITH UNIQUE args are printed
						 * separately, so ignore them here
						 */
						if (i > 2)
							break;

						appendStringInfoString(buf, " : ");
					}
					else
						appendStringInfoString(buf, ", ");
				}
				if (use_variadic && i == nargs)
					appendStringInfoString(buf, "VARIADIC ");
				get_rule_expr(arg, context, true);
			}
		}

		if (aggref->aggorder != NIL)
		{
			appendStringInfoString(buf, " ORDER BY ");
			get_rule_orderby(aggref->aggorder, aggref->args, false, context);
		}
	}

	if (options)
		appendStringInfoString(buf, options);

	if (aggref->aggfilter != NULL)
	{
		appendStringInfoString(buf, ") FILTER (WHERE ");
		get_rule_expr((Node *) aggref->aggfilter, context, false);
	}

	appendStringInfoChar(buf, ')');
}

/*
 * This is a helper function for get_agg_expr().  It's used when we deparse
 * a combining Aggref; resolve_special_varno locates the corresponding partial
 * Aggref and then calls this.
 */
static void
get_agg_combine_expr(Node *node, deparse_context *context, void *callback_arg)
{
	Aggref	   *aggref;
	Aggref	   *original_aggref = callback_arg;

	if (!IsA(node, Aggref))
		elog(ERROR, "combining Aggref does not point to an Aggref");

	aggref = (Aggref *) node;
	get_agg_expr(aggref, context, original_aggref);
}

/*
 * get_windowfunc_expr	- Parse back a WindowFunc node
 */
static void
get_windowfunc_expr(WindowFunc *wfunc, deparse_context *context)
{
	get_windowfunc_expr_helper(wfunc, context, NULL, NULL, false);
}


/*
 * get_windowfunc_expr_helper	- subroutine for get_windowfunc_expr and
 *								get_json_agg_constructor
 */
static void
get_windowfunc_expr_helper(WindowFunc *wfunc, deparse_context *context,
						   const char *funcname, const char *options,
						   bool is_json_objectagg)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[FUNC_MAX_ARGS];
	int			nargs;
	List	   *argnames;
	ListCell   *l;

	if (list_length(wfunc->args) > FUNC_MAX_ARGS)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_ARGUMENTS),
				 errmsg("too many arguments")));
	nargs = 0;
	argnames = NIL;
	foreach(l, wfunc->args)
	{
		Node	   *arg = (Node *) lfirst(l);

		if (IsA(arg, NamedArgExpr))
			argnames = lappend(argnames, ((NamedArgExpr *) arg)->name);
		argtypes[nargs] = exprType(arg);
		nargs++;
	}

	if (!funcname)
		funcname = generate_function_name(wfunc->winfnoid, nargs, argnames,
										  argtypes, false, NULL,
										  context->inGroupBy);

	appendStringInfo(buf, "%s(", funcname);

	/* winstar can be set only in zero-argument aggregates */
	if (wfunc->winstar)
		appendStringInfoChar(buf, '*');
	else
	{
		if (is_json_objectagg)
		{
			get_rule_expr((Node *) linitial(wfunc->args), context, false);
			appendStringInfoString(buf, " : ");
			get_rule_expr((Node *) lsecond(wfunc->args), context, false);
		}
		else
			get_rule_expr((Node *) wfunc->args, context, true);
	}

	if (options)
		appendStringInfoString(buf, options);

	if (wfunc->aggfilter != NULL)
	{
		appendStringInfoString(buf, ") FILTER (WHERE ");
		get_rule_expr((Node *) wfunc->aggfilter, context, false);
	}

	appendStringInfoString(buf, ") OVER ");

	foreach(l, context->windowClause)
	{
		WindowClause *wc = (WindowClause *) lfirst(l);

		if (wc->winref == wfunc->winref)
		{
			if (wc->name)
				appendStringInfoString(buf, quote_identifier(wc->name));
			else
				get_rule_windowspec(wc, context->targetList, context);
			break;
		}
	}
	if (l == NULL)
	{
		if (context->windowClause)
			elog(ERROR, "could not find window clause for winref %u",
				 wfunc->winref);

		/*
		 * In EXPLAIN, we don't have window context information available, so
		 * we have to settle for this:
		 */
		appendStringInfoString(buf, "(?)");
	}
}

/*
 * get_func_sql_syntax		- Parse back a SQL-syntax function call
 *
 * Returns true if we successfully deparsed, false if we did not
 * recognize the function.
 */
static bool
get_func_sql_syntax(FuncExpr *expr, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			funcoid = expr->funcid;

	switch (funcoid)
	{
		case F_TIMEZONE_INTERVAL_TIMESTAMP:
		case F_TIMEZONE_INTERVAL_TIMESTAMPTZ:
		case F_TIMEZONE_INTERVAL_TIMETZ:
		case F_TIMEZONE_TEXT_TIMESTAMP:
		case F_TIMEZONE_TEXT_TIMESTAMPTZ:
		case F_TIMEZONE_TEXT_TIMETZ:
			/* AT TIME ZONE ... note reversed argument order */
			appendStringInfoChar(buf, '(');
			get_rule_expr_paren((Node *) lsecond(expr->args), context, false,
								(Node *) expr);
			appendStringInfoString(buf, " AT TIME ZONE ");
			get_rule_expr_paren((Node *) linitial(expr->args), context, false,
								(Node *) expr);
			appendStringInfoChar(buf, ')');
			return true;

		case F_OVERLAPS_TIMESTAMPTZ_INTERVAL_TIMESTAMPTZ_INTERVAL:
		case F_OVERLAPS_TIMESTAMPTZ_INTERVAL_TIMESTAMPTZ_TIMESTAMPTZ:
		case F_OVERLAPS_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ_INTERVAL:
		case F_OVERLAPS_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ_TIMESTAMPTZ:
		case F_OVERLAPS_TIMESTAMP_INTERVAL_TIMESTAMP_INTERVAL:
		case F_OVERLAPS_TIMESTAMP_INTERVAL_TIMESTAMP_TIMESTAMP:
		case F_OVERLAPS_TIMESTAMP_TIMESTAMP_TIMESTAMP_INTERVAL:
		case F_OVERLAPS_TIMESTAMP_TIMESTAMP_TIMESTAMP_TIMESTAMP:
		case F_OVERLAPS_TIMETZ_TIMETZ_TIMETZ_TIMETZ:
		case F_OVERLAPS_TIME_INTERVAL_TIME_INTERVAL:
		case F_OVERLAPS_TIME_INTERVAL_TIME_TIME:
		case F_OVERLAPS_TIME_TIME_TIME_INTERVAL:
		case F_OVERLAPS_TIME_TIME_TIME_TIME:
			/* (x1, x2) OVERLAPS (y1, y2) */
			appendStringInfoString(buf, "((");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, ", ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, ") OVERLAPS (");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			appendStringInfoString(buf, ", ");
			get_rule_expr((Node *) lfourth(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;

		case F_EXTRACT_TEXT_DATE:
		case F_EXTRACT_TEXT_TIME:
		case F_EXTRACT_TEXT_TIMETZ:
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_EXTRACT_TEXT_INTERVAL:
			/* EXTRACT (x FROM y) */
			appendStringInfoString(buf, "EXTRACT(");
			{
				Const	   *con = (Const *) linitial(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfoString(buf, TextDatumGetCString(con->constvalue));
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_IS_NORMALIZED:
			/* IS xxx NORMALIZED */
			appendStringInfoString(buf, "(");
			get_rule_expr_paren((Node *) linitial(expr->args), context, false,
								(Node *) expr);
			appendStringInfoString(buf, " IS");
			if (list_length(expr->args) == 2)
			{
				Const	   *con = (Const *) lsecond(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfo(buf, " %s",
								 TextDatumGetCString(con->constvalue));
			}
			appendStringInfoString(buf, " NORMALIZED)");
			return true;

		case F_PG_COLLATION_FOR:
			/* COLLATION FOR */
			appendStringInfoString(buf, "COLLATION FOR (");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_NORMALIZE:
			/* NORMALIZE() */
			appendStringInfoString(buf, "NORMALIZE(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			if (list_length(expr->args) == 2)
			{
				Const	   *con = (Const *) lsecond(expr->args);

				Assert(IsA(con, Const) &&
					   con->consttype == TEXTOID &&
					   !con->constisnull);
				appendStringInfo(buf, ", %s",
								 TextDatumGetCString(con->constvalue));
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_OVERLAY_BIT_BIT_INT4:
		case F_OVERLAY_BIT_BIT_INT4_INT4:
		case F_OVERLAY_BYTEA_BYTEA_INT4:
		case F_OVERLAY_BYTEA_BYTEA_INT4_INT4:
		case F_OVERLAY_TEXT_TEXT_INT4:
		case F_OVERLAY_TEXT_TEXT_INT4_INT4:
			/* OVERLAY() */
			appendStringInfoString(buf, "OVERLAY(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " PLACING ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			if (list_length(expr->args) == 4)
			{
				appendStringInfoString(buf, " FOR ");
				get_rule_expr((Node *) lfourth(expr->args), context, false);
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_POSITION_BIT_BIT:
		case F_POSITION_BYTEA_BYTEA:
		case F_POSITION_TEXT_TEXT:
			/* POSITION() ... extra parens since args are b_expr not a_expr */
			appendStringInfoString(buf, "POSITION((");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, ") IN (");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;

		case F_SUBSTRING_BIT_INT4:
		case F_SUBSTRING_BIT_INT4_INT4:
		case F_SUBSTRING_BYTEA_INT4:
		case F_SUBSTRING_BYTEA_INT4_INT4:
		case F_SUBSTRING_TEXT_INT4:
		case F_SUBSTRING_TEXT_INT4_INT4:
			/* SUBSTRING FROM/FOR (i.e., integer-position variants) */
			appendStringInfoString(buf, "SUBSTRING(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			if (list_length(expr->args) == 3)
			{
				appendStringInfoString(buf, " FOR ");
				get_rule_expr((Node *) lthird(expr->args), context, false);
			}
			appendStringInfoChar(buf, ')');
			return true;

		case F_SUBSTRING_TEXT_TEXT_TEXT:
			/* SUBSTRING SIMILAR/ESCAPE */
			appendStringInfoString(buf, "SUBSTRING(");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, " SIMILAR ");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, " ESCAPE ");
			get_rule_expr((Node *) lthird(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_BTRIM_BYTEA_BYTEA:
		case F_BTRIM_TEXT:
		case F_BTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(BOTH");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_LTRIM_BYTEA_BYTEA:
		case F_LTRIM_TEXT:
		case F_LTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(LEADING");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_RTRIM_BYTEA_BYTEA:
		case F_RTRIM_TEXT:
		case F_RTRIM_TEXT_TEXT:
			/* TRIM() */
			appendStringInfoString(buf, "TRIM(TRAILING");
			if (list_length(expr->args) == 2)
			{
				appendStringInfoChar(buf, ' ');
				get_rule_expr((Node *) lsecond(expr->args), context, false);
			}
			appendStringInfoString(buf, " FROM ");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoChar(buf, ')');
			return true;

		case F_SYSTEM_USER:
			appendStringInfoString(buf, "SYSTEM_USER");
			return true;

		case F_XMLEXISTS:
			/* XMLEXISTS ... extra parens because args are c_expr */
			appendStringInfoString(buf, "XMLEXISTS((");
			get_rule_expr((Node *) linitial(expr->args), context, false);
			appendStringInfoString(buf, ") PASSING (");
			get_rule_expr((Node *) lsecond(expr->args), context, false);
			appendStringInfoString(buf, "))");
			return true;
	}
	return false;
}

/* ----------
 * get_coercion_expr
 *
 *	Make a string representation of a value coerced to a specific type
 * ----------
 */
static void
get_coercion_expr(Node *arg, deparse_context *context,
				  Oid resulttype, int32 resulttypmod,
				  Node *parentNode)
{
	StringInfo	buf = context->buf;

	/*
	 * Since parse_coerce.c doesn't immediately collapse application of
	 * length-coercion functions to constants, what we'll typically see in
	 * such cases is a Const with typmod -1 and a length-coercion function
	 * right above it.  Avoid generating redundant output. However, beware of
	 * suppressing casts when the user actually wrote something like
	 * 'foo'::text::char(3).
	 *
	 * Note: it might seem that we are missing the possibility of needing to
	 * print a COLLATE clause for such a Const.  However, a Const could only
	 * have nondefault collation in a post-constant-folding tree, in which the
	 * length coercion would have been folded too.  See also the special
	 * handling of CollateExpr in coerce_to_target_type(): any collation
	 * marking will be above the coercion node, not below it.
	 */
	if (arg && IsA(arg, Const) &&
		((Const *) arg)->consttype == resulttype &&
		((Const *) arg)->consttypmod == -1)
	{
		/* Show the constant without normal ::typename decoration */
		get_const_expr((Const *) arg, context, -1);
	}
	else
	{
		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, '(');
		get_rule_expr_paren(arg, context, false, parentNode);
		if (!PRETTY_PAREN(context))
			appendStringInfoChar(buf, ')');
	}

	/*
	 * Never emit resulttype(arg) functional notation. A pg_proc entry could
	 * take precedence, and a resulttype in pg_temp would require schema
	 * qualification that format_type_with_typemod() would usually omit. We've
	 * standardized on arg::resulttype, but CAST(arg AS resulttype) notation
	 * would work fine.
	 */
	appendStringInfo(buf, "::%s",
					 format_type_with_typemod(resulttype, resulttypmod));
}

/* ----------
 * get_const_expr
 *
 *	Make a string representation of a Const
 *
 * showtype can be -1 to never show "::typename" decoration, or +1 to always
 * show it, or 0 to show it only if the constant wouldn't be assumed to be
 * the right type by default.
 *
 * If the Const's collation isn't default for its type, show that too.
 * We mustn't do this when showtype is -1 (since that means the caller will
 * print "::typename", and we can't put a COLLATE clause in between).  It's
 * caller's responsibility that collation isn't missed in such cases.
 * ----------
 */
static void
get_const_expr(Const *constval, deparse_context *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval;
	bool		needlabel = false;

	if (constval->constisnull)
	{
		/*
		 * Always label the type of a NULL constant to prevent misdecisions
		 * about type when reparsing.
		 */
		appendStringInfoString(buf, "NULL");
		if (showtype >= 0)
		{
			appendStringInfo(buf, "::%s",
							 format_type_with_typemod(constval->consttype,
													  constval->consttypmod));
			get_const_collation(constval, context);
		}
		return;
	}

	getTypeOutputInfo(constval->consttype,
					  &typoutput, &typIsVarlena);

	extval = OidOutputFunctionCall(typoutput, constval->constvalue);

	switch (constval->consttype)
	{
		case INT4OID:

			/*
			 * INT4 can be printed without any decoration, unless it is
			 * negative; in that case print it as '-nnn'::integer to ensure
			 * that the output will re-parse as a constant, not as a constant
			 * plus operator.  In most cases we could get away with printing
			 * (-nnn) instead, because of the way that gram.y handles negative
			 * literals; but that doesn't work for INT_MIN, and it doesn't
			 * seem that much prettier anyway.
			 */
			if (extval[0] != '-')
				appendStringInfoString(buf, extval);
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case NUMERICOID:

			/*
			 * NUMERIC can be printed without quotes if it looks like a float
			 * constant (not an integer, and not Infinity or NaN) and doesn't
			 * have a leading sign (for the same reason as for INT4).
			 */
			if (isdigit((unsigned char) extval[0]) &&
				strcspn(extval, "eE.") != strlen(extval))
			{
				appendStringInfoString(buf, extval);
			}
			else
			{
				appendStringInfo(buf, "'%s'", extval);
				needlabel = true;	/* we must attach a cast */
			}
			break;

		case BOOLOID:
			if (strcmp(extval, "t") == 0)
				appendStringInfoString(buf, "true");
			else
				appendStringInfoString(buf, "false");
			break;

		default:
			simple_quote_literal(buf, extval);
			break;
	}

	pfree(extval);

	if (showtype < 0)
		return;

	/*
	 * For showtype == 0, append ::typename unless the constant will be
	 * implicitly typed as the right type when it is read in.
	 *
	 * XXX this code has to be kept in sync with the behavior of the parser,
	 * especially make_const.
	 */
	switch (constval->consttype)
	{
		case BOOLOID:
		case UNKNOWNOID:
			/* These types can be left unlabeled */
			needlabel = false;
			break;
		case INT4OID:
			/* We determined above whether a label is needed */
			break;
		case NUMERICOID:

			/*
			 * Float-looking constants will be typed as numeric, which we
			 * checked above; but if there's a nondefault typmod we need to
			 * show it.
			 */
			needlabel |= (constval->consttypmod >= 0);
			break;
		default:
			needlabel = true;
			break;
	}
	if (needlabel || showtype > 0)
		appendStringInfo(buf, "::%s",
						 format_type_with_typemod(constval->consttype,
												  constval->consttypmod));

	get_const_collation(constval, context);
}

/*
 * helper for get_const_expr: append COLLATE if needed
 */
static void
get_const_collation(Const *constval, deparse_context *context)
{
	StringInfo	buf = context->buf;

	if (OidIsValid(constval->constcollid))
	{
		Oid			typcollation = get_typcollation(constval->consttype);

		if (constval->constcollid != typcollation)
		{
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(constval->constcollid));
		}
	}
}

/*
 * get_json_format			- Parse back a JsonFormat node
 */
static void
get_json_format(JsonFormat *format, StringInfo buf)
{
	if (format->format_type == JS_FORMAT_DEFAULT)
		return;

	appendStringInfoString(buf,
						   format->format_type == JS_FORMAT_JSONB ?
						   " FORMAT JSONB" : " FORMAT JSON");

	if (format->encoding != JS_ENC_DEFAULT)
	{
		const char *encoding;

		encoding =
			format->encoding == JS_ENC_UTF16 ? "UTF16" :
			format->encoding == JS_ENC_UTF32 ? "UTF32" : "UTF8";

		appendStringInfo(buf, " ENCODING %s", encoding);
	}
}

/*
 * get_json_returning		- Parse back a JsonReturning structure
 */
static void
get_json_returning(JsonReturning *returning, StringInfo buf,
				   bool json_format_by_default)
{
	if (!OidIsValid(returning->typid))
		return;

	appendStringInfo(buf, " RETURNING %s",
					 format_type_with_typemod(returning->typid,
											  returning->typmod));

	if (!json_format_by_default ||
		returning->format->format_type !=
		(returning->typid == JSONBOID ? JS_FORMAT_JSONB : JS_FORMAT_JSON))
		get_json_format(returning->format, buf);
}

/*
 * get_json_constructor		- Parse back a JsonConstructorExpr node
 */
static void
get_json_constructor(JsonConstructorExpr *ctor, deparse_context *context,
					 bool showimplicit)
{
	StringInfo	buf = context->buf;
	const char *funcname;
	bool		is_json_object;
	int			curridx;
	ListCell   *lc;

	if (ctor->type == JSCTOR_JSON_OBJECTAGG)
	{
		get_json_agg_constructor(ctor, context, "JSON_OBJECTAGG", true);
		return;
	}
	else if (ctor->type == JSCTOR_JSON_ARRAYAGG)
	{
		get_json_agg_constructor(ctor, context, "JSON_ARRAYAGG", false);
		return;
	}

	switch (ctor->type)
	{
		case JSCTOR_JSON_OBJECT:
			funcname = "JSON_OBJECT";
			break;
		case JSCTOR_JSON_ARRAY:
			funcname = "JSON_ARRAY";
			break;
		default:
			elog(ERROR, "invalid JsonConstructorType %d", ctor->type);
	}

	appendStringInfo(buf, "%s(", funcname);

	is_json_object = ctor->type == JSCTOR_JSON_OBJECT;
	foreach(lc, ctor->args)
	{
		curridx = foreach_current_index(lc);
		if (curridx > 0)
		{
			const char *sep;

			sep = (is_json_object && (curridx % 2) != 0) ? " : " : ", ";
			appendStringInfoString(buf, sep);
		}

		get_rule_expr((Node *) lfirst(lc), context, true);
	}

	get_json_constructor_options(ctor, buf);
	appendStringInfo(buf, ")");
}

/*
 * Append options, if any, to the JSON constructor being deparsed
 */
static void
get_json_constructor_options(JsonConstructorExpr *ctor, StringInfo buf)
{
	if (ctor->absent_on_null)
	{
		if (ctor->type == JSCTOR_JSON_OBJECT ||
			ctor->type == JSCTOR_JSON_OBJECTAGG)
			appendStringInfoString(buf, " ABSENT ON NULL");
	}
	else
	{
		if (ctor->type == JSCTOR_JSON_ARRAY ||
			ctor->type == JSCTOR_JSON_ARRAYAGG)
			appendStringInfoString(buf, " NULL ON NULL");
	}

	if (ctor->unique)
		appendStringInfoString(buf, " WITH UNIQUE KEYS");

	get_json_returning(ctor->returning, buf, true);
}

/*
 * get_json_agg_constructor - Parse back an aggregate JsonConstructorExpr node
 */
static void
get_json_agg_constructor(JsonConstructorExpr *ctor, deparse_context *context,
						 const char *funcname, bool is_json_objectagg)
{
	StringInfoData options;

	initStringInfo(&options);
	get_json_constructor_options(ctor, &options);

	if (IsA(ctor->func, Aggref))
		get_agg_expr_helper((Aggref *) ctor->func, context,
							(Aggref *) ctor->func,
							funcname, options.data, is_json_objectagg);
	else if (IsA(ctor->func, WindowFunc))
		get_windowfunc_expr_helper((WindowFunc *) ctor->func, context,
								   funcname, options.data,
								   is_json_objectagg);
	else
		elog(ERROR, "invalid JsonConstructorExpr underlying node type: %d",
			 nodeTag(ctor->func));
}

/*
 * simple_quote_literal - Format a string as a SQL literal, append to buf
 */
static void
simple_quote_literal(StringInfo buf, const char *val)
{
	const char *valptr;

	/*
	 * We form the string literal according to the prevailing setting of
	 * standard_conforming_strings; we never use E''. User is responsible for
	 * making sure result is used correctly.
	 */
	appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, !standard_conforming_strings))
			appendStringInfoChar(buf, ch);
		appendStringInfoChar(buf, ch);
	}
	appendStringInfoChar(buf, '\'');
}


/* ----------
 * get_sublink_expr			- Parse back a sublink
 * ----------
 */
static void
get_sublink_expr(SubLink *sublink, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Query	   *query = (Query *) (sublink->subselect);
	char	   *opname = NULL;
	bool		need_paren;

	if (sublink->subLinkType == ARRAY_SUBLINK)
		appendStringInfoString(buf, "ARRAY(");
	else
		appendStringInfoChar(buf, '(');

	/*
	 * Note that we print the name of only the first operator, when there are
	 * multiple combining operators.  This is an approximation that could go
	 * wrong in various scenarios (operators in different schemas, renamed
	 * operators, etc) but there is not a whole lot we can do about it, since
	 * the syntax allows only one operator to be shown.
	 */
	if (sublink->testexpr)
	{
		if (IsA(sublink->testexpr, OpExpr))
		{
			/* single combining operator */
			OpExpr	   *opexpr = (OpExpr *) sublink->testexpr;

			get_rule_expr(linitial(opexpr->args), context, true);
			opname = generate_operator_name(opexpr->opno,
											exprType(linitial(opexpr->args)),
											exprType(lsecond(opexpr->args)));
		}
		else if (IsA(sublink->testexpr, BoolExpr))
		{
			/* multiple combining operators, = or <> cases */
			char	   *sep;
			ListCell   *l;

			appendStringInfoChar(buf, '(');
			sep = "";
			foreach(l, ((BoolExpr *) sublink->testexpr)->args)
			{
				OpExpr	   *opexpr = lfirst_node(OpExpr, l);

				appendStringInfoString(buf, sep);
				get_rule_expr(linitial(opexpr->args), context, true);
				if (!opname)
					opname = generate_operator_name(opexpr->opno,
													exprType(linitial(opexpr->args)),
													exprType(lsecond(opexpr->args)));
				sep = ", ";
			}
			appendStringInfoChar(buf, ')');
		}
		else if (IsA(sublink->testexpr, RowCompareExpr))
		{
			/* multiple combining operators, < <= > >= cases */
			RowCompareExpr *rcexpr = (RowCompareExpr *) sublink->testexpr;

			appendStringInfoChar(buf, '(');
			get_rule_expr((Node *) rcexpr->largs, context, true);
			opname = generate_operator_name(linitial_oid(rcexpr->opnos),
											exprType(linitial(rcexpr->largs)),
											exprType(linitial(rcexpr->rargs)));
			appendStringInfoChar(buf, ')');
		}
		else
			elog(ERROR, "unrecognized testexpr type: %d",
				 (int) nodeTag(sublink->testexpr));
	}

	need_paren = true;

	switch (sublink->subLinkType)
	{
		case EXISTS_SUBLINK:
			appendStringInfoString(buf, "EXISTS ");
			break;

		case ANY_SUBLINK:
			if (strcmp(opname, "=") == 0)	/* Represent = ANY as IN */
				appendStringInfoString(buf, " IN ");
			else
				appendStringInfo(buf, " %s ANY ", opname);
			break;

		case ALL_SUBLINK:
			appendStringInfo(buf, " %s ALL ", opname);
			break;

		case ROWCOMPARE_SUBLINK:
			appendStringInfo(buf, " %s ", opname);
			break;

		case EXPR_SUBLINK:
		case MULTIEXPR_SUBLINK:
		case ARRAY_SUBLINK:
			need_paren = false;
			break;

		case CTE_SUBLINK:		/* shouldn't occur in a SubLink */
		default:
			elog(ERROR, "unrecognized sublink type: %d",
				 (int) sublink->subLinkType);
			break;
	}

	if (need_paren)
		appendStringInfoChar(buf, '(');

	get_query_def(query, buf, context->namespaces, NULL, false,
				  context->prettyFlags, context->wrapColumn,
				  context->indentLevel);

	if (need_paren)
		appendStringInfoString(buf, "))");
	else
		appendStringInfoChar(buf, ')');
}


/* ----------
 * get_tablefunc			- Parse back a table function
 * ----------
 */
static void
get_tablefunc(TableFunc *tf, deparse_context *context, bool showimplicit)
{
	StringInfo	buf = context->buf;

	/* XMLTABLE is the only existing implementation.  */

	appendStringInfoString(buf, "XMLTABLE(");

	if (tf->ns_uris != NIL)
	{
		ListCell   *lc1,
				   *lc2;
		bool		first = true;

		appendStringInfoString(buf, "XMLNAMESPACES (");
		forboth(lc1, tf->ns_uris, lc2, tf->ns_names)
		{
			Node	   *expr = (Node *) lfirst(lc1);
			String	   *ns_node = lfirst_node(String, lc2);

			if (!first)
				appendStringInfoString(buf, ", ");
			else
				first = false;

			if (ns_node != NULL)
			{
				get_rule_expr(expr, context, showimplicit);
				appendStringInfo(buf, " AS %s",
								 quote_identifier(strVal(ns_node)));
			}
			else
			{
				appendStringInfoString(buf, "DEFAULT ");
				get_rule_expr(expr, context, showimplicit);
			}
		}
		appendStringInfoString(buf, "), ");
	}

	appendStringInfoChar(buf, '(');
	get_rule_expr((Node *) tf->rowexpr, context, showimplicit);
	appendStringInfoString(buf, ") PASSING (");
	get_rule_expr((Node *) tf->docexpr, context, showimplicit);
	appendStringInfoChar(buf, ')');

	if (tf->colexprs != NIL)
	{
		ListCell   *l1;
		ListCell   *l2;
		ListCell   *l3;
		ListCell   *l4;
		ListCell   *l5;
		int			colnum = 0;

		appendStringInfoString(buf, " COLUMNS ");
		forfive(l1, tf->colnames, l2, tf->coltypes, l3, tf->coltypmods,
				l4, tf->colexprs, l5, tf->coldefexprs)
		{
			char	   *colname = strVal(lfirst(l1));
			Oid			typid = lfirst_oid(l2);
			int32		typmod = lfirst_int(l3);
			Node	   *colexpr = (Node *) lfirst(l4);
			Node	   *coldefexpr = (Node *) lfirst(l5);
			bool		ordinality = (tf->ordinalitycol == colnum);
			bool		notnull = bms_is_member(colnum, tf->notnulls);

			if (colnum > 0)
				appendStringInfoString(buf, ", ");
			colnum++;

			appendStringInfo(buf, "%s %s", quote_identifier(colname),
							 ordinality ? "FOR ORDINALITY" :
							 format_type_with_typemod(typid, typmod));
			if (ordinality)
				continue;

			if (coldefexpr != NULL)
			{
				appendStringInfoString(buf, " DEFAULT (");
				get_rule_expr((Node *) coldefexpr, context, showimplicit);
				appendStringInfoChar(buf, ')');
			}
			if (colexpr != NULL)
			{
				appendStringInfoString(buf, " PATH (");
				get_rule_expr((Node *) colexpr, context, showimplicit);
				appendStringInfoChar(buf, ')');
			}
			if (notnull)
				appendStringInfoString(buf, " NOT NULL");
		}
	}

	appendStringInfoChar(buf, ')');
}

/* ----------
 * get_from_clause			- Parse back a FROM clause
 *
 * "prefix" is the keyword that denotes the start of the list of FROM
 * elements. It is FROM when used to parse back SELECT and UPDATE, but
 * is USING when parsing back DELETE.
 * ----------
 */
static void
get_from_clause(Query *query, const char *prefix, deparse_context *context)
{
	StringInfo	buf = context->buf;
	bool		first = true;
	ListCell   *l;

	/*
	 * We use the query's jointree as a guide to what to print.  However, we
	 * must ignore auto-added RTEs that are marked not inFromCl. (These can
	 * only appear at the top level of the jointree, so it's sufficient to
	 * check here.)  This check also ensures we ignore the rule pseudo-RTEs
	 * for NEW and OLD.
	 */
	foreach(l, query->jointree->fromlist)
	{
		Node	   *jtnode = (Node *) lfirst(l);

		if (IsA(jtnode, RangeTblRef))
		{
			int			varno = ((RangeTblRef *) jtnode)->rtindex;
			RangeTblEntry *rte = rt_fetch(varno, query->rtable);

			if (!rte->inFromCl)
				continue;
		}

		if (first)
		{
			appendContextKeyword(context, prefix,
								 -PRETTYINDENT_STD, PRETTYINDENT_STD, 2);
			first = false;

			get_from_clause_item(jtnode, query, context);
		}
		else
		{
			StringInfoData itembuf;

			appendStringInfoString(buf, ", ");

			/*
			 * Put the new FROM item's text into itembuf so we can decide
			 * after we've got it whether or not it needs to go on a new line.
			 */
			initStringInfo(&itembuf);
			context->buf = &itembuf;

			get_from_clause_item(jtnode, query, context);

			/* Restore context's output buffer */
			context->buf = buf;

			/* Consider line-wrapping if enabled */
			if (PRETTY_INDENT(context) && context->wrapColumn >= 0)
			{
				/* Does the new item start with a new line? */
				if (itembuf.len > 0 && itembuf.data[0] == '\n')
				{
					/* If so, we shouldn't add anything */
					/* instead, remove any trailing spaces currently in buf */
					removeStringInfoSpaces(buf);
				}
				else
				{
					char	   *trailing_nl;

					/* Locate the start of the current line in the buffer */
					trailing_nl = strrchr(buf->data, '\n');
					if (trailing_nl == NULL)
						trailing_nl = buf->data;
					else
						trailing_nl++;

					/*
					 * Add a newline, plus some indentation, if the new item
					 * would cause an overflow.
					 */
					if (strlen(trailing_nl) + itembuf.len > context->wrapColumn)
						appendContextKeyword(context, "", -PRETTYINDENT_STD,
											 PRETTYINDENT_STD,
											 PRETTYINDENT_VAR);
				}
			}

			/* Add the new item */
			appendBinaryStringInfo(buf, itembuf.data, itembuf.len);

			/* clean up */
			pfree(itembuf.data);
		}
	}
}

static void
get_from_clause_item(Node *jtnode, Query *query, deparse_context *context)
{
	StringInfo	buf = context->buf;
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);
		deparse_columns *colinfo = deparse_columns_fetch(varno, dpns);
		RangeTblFunction *rtfunc1 = NULL;

		if (rte->lateral)
			appendStringInfoString(buf, "LATERAL ");

		/* Print the FROM item proper */
		switch (rte->rtekind)
		{
			case RTE_RELATION:
				/* Normal relation RTE */
				appendStringInfo(buf, "%s%s",
								 only_marker(rte),
								 generate_relation_name(rte->relid,
														context->namespaces));
				break;
			case RTE_SUBQUERY:
				/* Subquery RTE */
				appendStringInfoChar(buf, '(');
				get_query_def(rte->subquery, buf, context->namespaces, NULL,
							  true,
							  context->prettyFlags, context->wrapColumn,
							  context->indentLevel);
				appendStringInfoChar(buf, ')');
				break;
			case RTE_FUNCTION:
				/* Function RTE */
				rtfunc1 = (RangeTblFunction *) linitial(rte->functions);

				/*
				 * Omit ROWS FROM() syntax for just one function, unless it
				 * has both a coldeflist and WITH ORDINALITY. If it has both,
				 * we must use ROWS FROM() syntax to avoid ambiguity about
				 * whether the coldeflist includes the ordinality column.
				 */
				if (list_length(rte->functions) == 1 &&
					(rtfunc1->funccolnames == NIL || !rte->funcordinality))
				{
					get_rule_expr_funccall(rtfunc1->funcexpr, context, true);
					/* we'll print the coldeflist below, if it has one */
				}
				else
				{
					bool		all_unnest;
					ListCell   *lc;

					/*
					 * If all the function calls in the list are to unnest,
					 * and none need a coldeflist, then collapse the list back
					 * down to UNNEST(args).  (If we had more than one
					 * built-in unnest function, this would get more
					 * difficult.)
					 *
					 * XXX This is pretty ugly, since it makes not-terribly-
					 * future-proof assumptions about what the parser would do
					 * with the output; but the alternative is to emit our
					 * nonstandard ROWS FROM() notation for what might have
					 * been a perfectly spec-compliant multi-argument
					 * UNNEST().
					 */
					all_unnest = true;
					foreach(lc, rte->functions)
					{
						RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

						if (!IsA(rtfunc->funcexpr, FuncExpr) ||
							((FuncExpr *) rtfunc->funcexpr)->funcid != F_UNNEST_ANYARRAY ||
							rtfunc->funccolnames != NIL)
						{
							all_unnest = false;
							break;
						}
					}

					if (all_unnest)
					{
						List	   *allargs = NIL;

						foreach(lc, rte->functions)
						{
							RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);
							List	   *args = ((FuncExpr *) rtfunc->funcexpr)->args;

							allargs = list_concat(allargs, args);
						}

						appendStringInfoString(buf, "UNNEST(");
						get_rule_expr((Node *) allargs, context, true);
						appendStringInfoChar(buf, ')');
					}
					else
					{
						int			funcno = 0;

						appendStringInfoString(buf, "ROWS FROM(");
						foreach(lc, rte->functions)
						{
							RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(lc);

							if (funcno > 0)
								appendStringInfoString(buf, ", ");
							get_rule_expr_funccall(rtfunc->funcexpr, context, true);
							if (rtfunc->funccolnames != NIL)
							{
								/* Reconstruct the column definition list */
								appendStringInfoString(buf, " AS ");
								get_from_clause_coldeflist(rtfunc,
														   NULL,
														   context);
							}
							funcno++;
						}
						appendStringInfoChar(buf, ')');
					}
					/* prevent printing duplicate coldeflist below */
					rtfunc1 = NULL;
				}
				if (rte->funcordinality)
					appendStringInfoString(buf, " WITH ORDINALITY");
				break;
			case RTE_TABLEFUNC:
				get_tablefunc(rte->tablefunc, context, true);
				break;
			case RTE_VALUES:
				/* Values list RTE */
				appendStringInfoChar(buf, '(');
				get_values_def(rte->values_lists, context);
				appendStringInfoChar(buf, ')');
				break;
			case RTE_CTE:
				appendStringInfoString(buf, quote_identifier(rte->ctename));
				break;
			default:
				elog(ERROR, "unrecognized RTE kind: %d", (int) rte->rtekind);
				break;
		}

		/* Print the relation alias, if needed */
		get_rte_alias(rte, varno, false, context);

		/* Print the column definitions or aliases, if needed */
		if (rtfunc1 && rtfunc1->funccolnames != NIL)
		{
			/* Reconstruct the columndef list, which is also the aliases */
			get_from_clause_coldeflist(rtfunc1, colinfo, context);
		}
		else
		{
			/* Else print column aliases as needed */
			get_column_alias_list(colinfo, context);
		}

		/* Tablesample clause must go after any alias */
		if (rte->rtekind == RTE_RELATION && rte->tablesample)
			get_tablesample_def(rte->tablesample, context);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		deparse_columns *colinfo = deparse_columns_fetch(j->rtindex, dpns);
		bool		need_paren_on_right;

		need_paren_on_right = PRETTY_PAREN(context) &&
			!IsA(j->rarg, RangeTblRef) &&
			!(IsA(j->rarg, JoinExpr) && ((JoinExpr *) j->rarg)->alias != NULL);

		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, '(');

		get_from_clause_item(j->larg, query, context);

		switch (j->jointype)
		{
			case JOIN_INNER:
				if (j->quals)
					appendContextKeyword(context, " JOIN ",
										 -PRETTYINDENT_STD,
										 PRETTYINDENT_STD,
										 PRETTYINDENT_JOIN);
				else
					appendContextKeyword(context, " CROSS JOIN ",
										 -PRETTYINDENT_STD,
										 PRETTYINDENT_STD,
										 PRETTYINDENT_JOIN);
				break;
			case JOIN_LEFT:
				appendContextKeyword(context, " LEFT JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			case JOIN_FULL:
				appendContextKeyword(context, " FULL JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			case JOIN_RIGHT:
				appendContextKeyword(context, " RIGHT JOIN ",
									 -PRETTYINDENT_STD,
									 PRETTYINDENT_STD,
									 PRETTYINDENT_JOIN);
				break;
			default:
				elog(ERROR, "unrecognized join type: %d",
					 (int) j->jointype);
		}

		if (need_paren_on_right)
			appendStringInfoChar(buf, '(');
		get_from_clause_item(j->rarg, query, context);
		if (need_paren_on_right)
			appendStringInfoChar(buf, ')');

		if (j->usingClause)
		{
			ListCell   *lc;
			bool		first = true;

			appendStringInfoString(buf, " USING (");
			/* Use the assigned names, not what's in usingClause */
			foreach(lc, colinfo->usingNames)
			{
				char	   *colname = (char *) lfirst(lc);

				if (first)
					first = false;
				else
					appendStringInfoString(buf, ", ");
				appendStringInfoString(buf, quote_identifier(colname));
			}
			appendStringInfoChar(buf, ')');

			if (j->join_using_alias)
				appendStringInfo(buf, " AS %s",
								 quote_identifier(j->join_using_alias->aliasname));
		}
		else if (j->quals)
		{
			appendStringInfoString(buf, " ON ");
			if (!PRETTY_PAREN(context))
				appendStringInfoChar(buf, '(');
			get_rule_expr(j->quals, context, false);
			if (!PRETTY_PAREN(context))
				appendStringInfoChar(buf, ')');
		}
		else if (j->jointype != JOIN_INNER)
		{
			/* If we didn't say CROSS JOIN above, we must provide an ON */
			appendStringInfoString(buf, " ON TRUE");
		}

		if (!PRETTY_PAREN(context) || j->alias != NULL)
			appendStringInfoChar(buf, ')');

		/* Yes, it's correct to put alias after the right paren ... */
		if (j->alias != NULL)
		{
			/*
			 * Note that it's correct to emit an alias clause if and only if
			 * there was one originally.  Otherwise we'd be converting a named
			 * join to unnamed or vice versa, which creates semantic
			 * subtleties we don't want.  However, we might print a different
			 * alias name than was there originally.
			 */
			appendStringInfo(buf, " %s",
							 quote_identifier(get_rtable_name(j->rtindex,
															  context)));
			get_column_alias_list(colinfo, context);
		}
	}
	else
		elog(ERROR, "unrecognized node type: %d",
			 (int) nodeTag(jtnode));
}

/*
 * get_rte_alias - print the relation's alias, if needed
 *
 * If printed, the alias is preceded by a space, or by " AS " if use_as is true.
 */
static void
get_rte_alias(RangeTblEntry *rte, int varno, bool use_as,
			  deparse_context *context)
{
	deparse_namespace *dpns = (deparse_namespace *) linitial(context->namespaces);
	char	   *refname = get_rtable_name(varno, context);
	deparse_columns *colinfo = deparse_columns_fetch(varno, dpns);
	bool		printalias = false;

	if (rte->alias != NULL)
	{
		/* Always print alias if user provided one */
		printalias = true;
	}
	else if (colinfo->printaliases)
	{
		/* Always print alias if we need to print column aliases */
		printalias = true;
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * No need to print alias if it's same as relation name (this would
		 * normally be the case, but not if set_rtable_names had to resolve a
		 * conflict).
		 */
		if (strcmp(refname, get_relation_name(rte->relid)) != 0)
			printalias = true;
	}
	else if (rte->rtekind == RTE_FUNCTION)
	{
		/*
		 * For a function RTE, always print alias.  This covers possible
		 * renaming of the function and/or instability of the FigureColname
		 * rules for things that aren't simple functions.  Note we'd need to
		 * force it anyway for the columndef list case.
		 */
		printalias = true;
	}
	else if (rte->rtekind == RTE_SUBQUERY ||
			 rte->rtekind == RTE_VALUES)
	{
		/*
		 * For a subquery, always print alias.  This makes the output
		 * SQL-spec-compliant, even though we allow such aliases to be omitted
		 * on input.
		 */
		printalias = true;
	}
	else if (rte->rtekind == RTE_CTE)
	{
		/*
		 * No need to print alias if it's same as CTE name (this would
		 * normally be the case, but not if set_rtable_names had to resolve a
		 * conflict).
		 */
		if (strcmp(refname, rte->ctename) != 0)
			printalias = true;
	}

	if (printalias)
		appendStringInfo(context->buf, "%s%s",
						 use_as ? " AS " : " ",
						 quote_identifier(refname));
}

/*
 * get_column_alias_list - print column alias list for an RTE
 *
 * Caller must already have printed the relation's alias name.
 */
static void
get_column_alias_list(deparse_columns *colinfo, deparse_context *context)
{
	StringInfo	buf = context->buf;
	int			i;
	bool		first = true;

	/* Don't print aliases if not needed */
	if (!colinfo->printaliases)
		return;

	for (i = 0; i < colinfo->num_new_cols; i++)
	{
		char	   *colname = colinfo->new_colnames[i];

		if (first)
		{
			appendStringInfoChar(buf, '(');
			first = false;
		}
		else
			appendStringInfoString(buf, ", ");
		appendStringInfoString(buf, quote_identifier(colname));
	}
	if (!first)
		appendStringInfoChar(buf, ')');
}

/*
 * get_from_clause_coldeflist - reproduce FROM clause coldeflist
 *
 * When printing a top-level coldeflist (which is syntactically also the
 * relation's column alias list), use column names from colinfo.  But when
 * printing a coldeflist embedded inside ROWS FROM(), we prefer to use the
 * original coldeflist's names, which are available in rtfunc->funccolnames.
 * Pass NULL for colinfo to select the latter behavior.
 *
 * The coldeflist is appended immediately (no space) to buf.  Caller is
 * responsible for ensuring that an alias or AS is present before it.
 */
static void
get_from_clause_coldeflist(RangeTblFunction *rtfunc,
						   deparse_columns *colinfo,
						   deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;
	int			i;

	appendStringInfoChar(buf, '(');

	i = 0;
	forfour(l1, rtfunc->funccoltypes,
			l2, rtfunc->funccoltypmods,
			l3, rtfunc->funccolcollations,
			l4, rtfunc->funccolnames)
	{
		Oid			atttypid = lfirst_oid(l1);
		int32		atttypmod = lfirst_int(l2);
		Oid			attcollation = lfirst_oid(l3);
		char	   *attname;

		if (colinfo)
			attname = colinfo->colnames[i];
		else
			attname = strVal(lfirst(l4));

		Assert(attname);		/* shouldn't be any dropped columns here */

		if (i > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfo(buf, "%s %s",
						 quote_identifier(attname),
						 format_type_with_typemod(atttypid, atttypmod));
		if (OidIsValid(attcollation) &&
			attcollation != get_typcollation(atttypid))
			appendStringInfo(buf, " COLLATE %s",
							 generate_collation_name(attcollation));

		i++;
	}

	appendStringInfoChar(buf, ')');
}

/*
 * get_tablesample_def			- print a TableSampleClause
 */
static void
get_tablesample_def(TableSampleClause *tablesample, deparse_context *context)
{
	StringInfo	buf = context->buf;
	Oid			argtypes[1];
	int			nargs;
	ListCell   *l;

	/*
	 * We should qualify the handler's function name if it wouldn't be
	 * resolved by lookup in the current search path.
	 */
	argtypes[0] = INTERNALOID;
	appendStringInfo(buf, " TABLESAMPLE %s (",
					 generate_function_name(tablesample->tsmhandler, 1,
											NIL, argtypes,
											false, NULL, false));

	nargs = 0;
	foreach(l, tablesample->args)
	{
		if (nargs++ > 0)
			appendStringInfoString(buf, ", ");
		get_rule_expr((Node *) lfirst(l), context, false);
	}
	appendStringInfoChar(buf, ')');

	if (tablesample->repeatable != NULL)
	{
		appendStringInfoString(buf, " REPEATABLE (");
		get_rule_expr((Node *) tablesample->repeatable, context, false);
		appendStringInfoChar(buf, ')');
	}
}

/*
 * get_opclass_name			- fetch name of an index operator class
 *
 * The opclass name is appended (after a space) to buf.
 *
 * Output is suppressed if the opclass is the default for the given
 * actual_datatype.  (If you don't want this behavior, just pass
 * InvalidOid for actual_datatype.)
 */
static void
get_opclass_name(Oid opclass, Oid actual_datatype,
				 StringInfo buf)
{
	HeapTuple	ht_opc;
	Form_pg_opclass opcrec;
	char	   *opcname;
	char	   *nspname;

	ht_opc = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
	if (!HeapTupleIsValid(ht_opc))
		elog(ERROR, "cache lookup failed for opclass %u", opclass);
	opcrec = (Form_pg_opclass) GETSTRUCT(ht_opc);

	if (!OidIsValid(actual_datatype) ||
		GetDefaultOpClass(actual_datatype, opcrec->opcmethod) != opclass)
	{
		/* Okay, we need the opclass name.  Do we need to qualify it? */
		opcname = NameStr(opcrec->opcname);
		if (OpclassIsVisible(opclass))
			appendStringInfo(buf, " %s", quote_identifier(opcname));
		else
		{
			nspname = get_namespace_name_or_temp(opcrec->opcnamespace);
			appendStringInfo(buf, " %s.%s",
							 quote_identifier(nspname),
							 quote_identifier(opcname));
		}
	}
	ReleaseSysCache(ht_opc);
}

/*
 * generate_opclass_name
 *		Compute the name to display for an opclass specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
char *
generate_opclass_name(Oid opclass)
{
	StringInfoData buf;

	initStringInfo(&buf);
	get_opclass_name(opclass, InvalidOid, &buf);

	return &buf.data[1];		/* get_opclass_name() prepends space */
}

/*
 * processIndirection - take care of array and subfield assignment
 *
 * We strip any top-level FieldStore or assignment SubscriptingRef nodes that
 * appear in the input, printing them as decoration for the base column
 * name (which we assume the caller just printed).  We might also need to
 * strip CoerceToDomain nodes, but only ones that appear above assignment
 * nodes.
 *
 * Returns the subexpression that's to be assigned.
 */
static Node *
processIndirection(Node *node, deparse_context *context)
{
	StringInfo	buf = context->buf;
	CoerceToDomain *cdomain = NULL;

	for (;;)
	{
		if (node == NULL)
			break;
		if (IsA(node, FieldStore))
		{
			FieldStore *fstore = (FieldStore *) node;
			Oid			typrelid;
			char	   *fieldname;

			/* lookup tuple type */
			typrelid = get_typ_typrelid(fstore->resulttype);
			if (!OidIsValid(typrelid))
				elog(ERROR, "argument type %s of FieldStore is not a tuple type",
					 format_type_be(fstore->resulttype));

			/*
			 * Print the field name.  There should only be one target field in
			 * stored rules.  There could be more than that in executable
			 * target lists, but this function cannot be used for that case.
			 */
			Assert(list_length(fstore->fieldnums) == 1);
			fieldname = get_attname(typrelid,
									linitial_int(fstore->fieldnums), false);
			appendStringInfo(buf, ".%s", quote_identifier(fieldname));

			/*
			 * We ignore arg since it should be an uninteresting reference to
			 * the target column or subcolumn.
			 */
			node = (Node *) linitial(fstore->newvals);
		}
		else if (IsA(node, SubscriptingRef))
		{
			SubscriptingRef *sbsref = (SubscriptingRef *) node;

			if (sbsref->refassgnexpr == NULL)
				break;

			printSubscripts(sbsref, context);

			/*
			 * We ignore refexpr since it should be an uninteresting reference
			 * to the target column or subcolumn.
			 */
			node = (Node *) sbsref->refassgnexpr;
		}
		else if (IsA(node, CoerceToDomain))
		{
			cdomain = (CoerceToDomain *) node;
			/* If it's an explicit domain coercion, we're done */
			if (cdomain->coercionformat != COERCE_IMPLICIT_CAST)
				break;
			/* Tentatively descend past the CoerceToDomain */
			node = (Node *) cdomain->arg;
		}
		else
			break;
	}

	/*
	 * If we descended past a CoerceToDomain whose argument turned out not to
	 * be a FieldStore or array assignment, back up to the CoerceToDomain.
	 * (This is not enough to be fully correct if there are nested implicit
	 * CoerceToDomains, but such cases shouldn't ever occur.)
	 */
	if (cdomain && node == (Node *) cdomain->arg)
		node = (Node *) cdomain;

	return node;
}

static void
printSubscripts(SubscriptingRef *sbsref, deparse_context *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	lowlist_item = list_head(sbsref->reflowerindexpr);	/* could be NULL */
	foreach(uplist_item, sbsref->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			/* If subexpression is NULL, get_rule_expr prints nothing */
			get_rule_expr((Node *) lfirst(lowlist_item), context, false);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext(sbsref->reflowerindexpr, lowlist_item);
		}
		/* If subexpression is NULL, get_rule_expr prints nothing */
		get_rule_expr((Node *) lfirst(uplist_item), context, false);
		appendStringInfoChar(buf, ']');
	}
}

/*
 * quote_identifier			- Quote an identifier only if needed
 *
 * When quotes are needed, we palloc the required space; slightly
 * space-wasteful but well worth it for notational simplicity.
 */
const char *
quote_identifier(const char *ident)
{
	/*
	 * Can avoid quoting if ident starts with a lowercase letter or underscore
	 * and contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply quotes.
	 */
	int			nquotes = 0;
	bool		safe;
	const char *ptr;
	char	   *result;
	char	   *optr;

	/*
	 * would like to use <ctype.h> macros here, but they might yield unwanted
	 * locale-specific results...
	 */
	safe = ((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_');

	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			(ch == '_'))
		{
			/* okay */
		}
		else
		{
			safe = false;
			if (ch == '"')
				nquotes++;
		}
	}

	if (quote_all_identifiers)
		safe = false;

	if (safe)
	{
		/*
		 * Check for keyword.  We quote keywords except for unreserved ones.
		 * (In some cases we could avoid quoting a col_name or type_func_name
		 * keyword, but it seems much harder than it's worth to tell that.)
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */
		int			kwnum = ScanKeywordLookup(ident, &ScanKeywords);

		if (kwnum >= 0 && ScanKeywordCategories[kwnum] != UNRESERVED_KEYWORD)
			safe = false;
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + nquotes + 2 + 1);

	optr = result;
	*optr++ = '"';
	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '"')
			*optr++ = '"';
		*optr++ = ch;
	}
	*optr++ = '"';
	*optr = '\0';

	return result;
}

/*
 * quote_qualified_identifier	- Quote a possibly-qualified identifier
 *
 * Return a name of the form qualifier.ident, or just ident if qualifier
 * is NULL, quoting each component if necessary.  The result is palloc'd.
 */
char *
quote_qualified_identifier(const char *qualifier,
						   const char *ident)
{
	StringInfoData buf;

	initStringInfo(&buf);
	if (qualifier)
		appendStringInfo(&buf, "%s.", quote_identifier(qualifier));
	appendStringInfoString(&buf, quote_identifier(ident));
	return buf.data;
}

/*
 * get_relation_name
 *		Get the unqualified name of a relation specified by OID
 *
 * This differs from the underlying get_rel_name() function in that it will
 * throw error instead of silently returning NULL if the OID is bad.
 */
static char *
get_relation_name(Oid relid)
{
	char	   *relname = get_rel_name(relid);

	if (!relname)
		elog(ERROR, "cache lookup failed for relation %u", relid);
	return relname;
}

/*
 * generate_relation_name
 *		Compute the name to display for a relation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 *
 * If namespaces isn't NIL, it must be a list of deparse_namespace nodes.
 * We will forcibly qualify the relation name if it equals any CTE name
 * visible in the namespace list.
 */
static char *
generate_relation_name(Oid relid, List *namespaces)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	bool		need_qual;
	ListCell   *nslist;
	char	   *relname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);
	relname = NameStr(reltup->relname);

	/* Check for conflicting CTE name */
	need_qual = false;
	foreach(nslist, namespaces)
	{
		deparse_namespace *dpns = (deparse_namespace *) lfirst(nslist);
		ListCell   *ctlist;

		foreach(ctlist, dpns->ctes)
		{
			CommonTableExpr *cte = (CommonTableExpr *) lfirst(ctlist);

			if (strcmp(cte->ctename, relname) == 0)
			{
				need_qual = true;
				break;
			}
		}
		if (need_qual)
			break;
	}

	/* Otherwise, qualify the name if not visible in search path */
	if (!need_qual)
		need_qual = !RelationIsVisible(relid);

	if (need_qual)
		nspname = get_namespace_name_or_temp(reltup->relnamespace);
	else
		nspname = NULL;

	result = quote_qualified_identifier(nspname, relname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_qualified_relation_name
 *		Compute the name to display for a relation specified by OID
 *
 * As above, but unconditionally schema-qualify the name.
 */
static char *
generate_qualified_relation_name(Oid relid)
{
	HeapTuple	tp;
	Form_pg_class reltup;
	char	   *relname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	reltup = (Form_pg_class) GETSTRUCT(tp);
	relname = NameStr(reltup->relname);

	nspname = get_namespace_name_or_temp(reltup->relnamespace);
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u",
			 reltup->relnamespace);

	result = quote_qualified_identifier(nspname, relname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_function_name
 *		Compute the name to display for a function specified by OID,
 *		given that it is being called with the specified actual arg names and
 *		types.  (Those matter because of ambiguous-function resolution rules.)
 *
 * If we're dealing with a potentially variadic function (in practice, this
 * means a FuncExpr or Aggref, not some other way of calling a function), then
 * has_variadic must specify whether variadic arguments have been merged,
 * and *use_variadic_p will be set to indicate whether to print VARIADIC in
 * the output.  For non-FuncExpr cases, has_variadic should be false and
 * use_variadic_p can be NULL.
 *
 * inGroupBy must be true if we're deparsing a GROUP BY clause.
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
static char *
generate_function_name(Oid funcid, int nargs, List *argnames, Oid *argtypes,
					   bool has_variadic, bool *use_variadic_p,
					   bool inGroupBy)
{
	char	   *result;
	HeapTuple	proctup;
	Form_pg_proc procform;
	char	   *proname;
	bool		use_variadic;
	char	   *nspname;
	FuncDetailCode p_result;
	Oid			p_funcid;
	Oid			p_rettype;
	bool		p_retset;
	int			p_nvargs;
	Oid			p_vatype;
	Oid		   *p_true_typeids;
	bool		force_qualify = false;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procform->proname);

	/*
	 * Due to parser hacks to avoid needing to reserve CUBE, we need to force
	 * qualification of some function names within GROUP BY.
	 */
	if (inGroupBy)
	{
		if (strcmp(proname, "cube") == 0 || strcmp(proname, "rollup") == 0)
			force_qualify = true;
	}

	/*
	 * Determine whether VARIADIC should be printed.  We must do this first
	 * since it affects the lookup rules in func_get_detail().
	 *
	 * We always print VARIADIC if the function has a merged variadic-array
	 * argument.  Note that this is always the case for functions taking a
	 * VARIADIC argument type other than VARIADIC ANY.  If we omitted VARIADIC
	 * and printed the array elements as separate arguments, the call could
	 * match a newer non-VARIADIC function.
	 */
	if (use_variadic_p)
	{
		/* Parser should not have set funcvariadic unless fn is variadic */
		Assert(!has_variadic || OidIsValid(procform->provariadic));
		use_variadic = has_variadic;
		*use_variadic_p = use_variadic;
	}
	else
	{
		Assert(!has_variadic);
		use_variadic = false;
	}

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct function given the unqualified func name with the
	 * specified argtypes and VARIADIC flag.  But if we already decided to
	 * force qualification, then we can skip the lookup and pretend we didn't
	 * find it.
	 */
	if (!force_qualify)
		p_result = func_get_detail(list_make1(makeString(proname)),
								   NIL, argnames, nargs, argtypes,
								   !use_variadic, true, false,
								   &p_funcid, &p_rettype,
								   &p_retset, &p_nvargs, &p_vatype,
								   &p_true_typeids, NULL);
	else
	{
		p_result = FUNCDETAIL_NOTFOUND;
		p_funcid = InvalidOid;
	}

	if ((p_result == FUNCDETAIL_NORMAL ||
		 p_result == FUNCDETAIL_AGGREGATE ||
		 p_result == FUNCDETAIL_WINDOWFUNC) &&
		p_funcid == funcid)
		nspname = NULL;
	else
		nspname = get_namespace_name_or_temp(procform->pronamespace);

	result = quote_qualified_identifier(nspname, proname);

	ReleaseSysCache(proctup);

	return result;
}

/*
 * generate_operator_name
 *		Compute the name to display for an operator specified by OID,
 *		given that it is being called with the specified actual arg types.
 *		(Arg types matter because of ambiguous-operator resolution rules.
 *		Pass InvalidOid for unused arg of a unary operator.)
 *
 * The result includes all necessary quoting and schema-prefixing,
 * plus the OPERATOR() decoration needed to use a qualified operator name
 * in an expression.
 */
static char *
generate_operator_name(Oid operid, Oid arg1, Oid arg2)
{
	StringInfoData buf;
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;
	Operator	p_result;

	initStringInfo(&buf);

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operid));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", operid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	oprname = NameStr(operform->oprname);

	/*
	 * The idea here is to schema-qualify only if the parser would fail to
	 * resolve the correct operator given the unqualified op name with the
	 * specified argtypes.
	 */
	switch (operform->oprkind)
	{
		case 'b':
			p_result = oper(NULL, list_make1(makeString(oprname)), arg1, arg2,
							true, -1);
			break;
		case 'l':
			p_result = left_oper(NULL, list_make1(makeString(oprname)), arg2,
								 true, -1);
			break;
		default:
			elog(ERROR, "unrecognized oprkind: %d", operform->oprkind);
			p_result = NULL;	/* keep compiler quiet */
			break;
	}

	if (p_result != NULL && oprid(p_result) == operid)
		nspname = NULL;
	else
	{
		nspname = get_namespace_name_or_temp(operform->oprnamespace);
		appendStringInfo(&buf, "OPERATOR(%s.", quote_identifier(nspname));
	}

	appendStringInfoString(&buf, oprname);

	if (nspname)
		appendStringInfoChar(&buf, ')');

	if (p_result != NULL)
		ReleaseSysCache(p_result);

	ReleaseSysCache(opertup);

	return buf.data;
}

/*
 * generate_operator_clause --- generate a binary-operator WHERE clause
 *
 * This is used for internally-generated-and-executed SQL queries, where
 * precision is essential and readability is secondary.  The basic
 * requirement is to append "leftop op rightop" to buf, where leftop and
 * rightop are given as strings and are assumed to yield types leftoptype
 * and rightoptype; the operator is identified by OID.  The complexity
 * comes from needing to be sure that the parser will select the desired
 * operator when the query is parsed.  We always name the operator using
 * OPERATOR(schema.op) syntax, so as to avoid search-path uncertainties.
 * We have to emit casts too, if either input isn't already the input type
 * of the operator; else we are at the mercy of the parser's heuristics for
 * ambiguous-operator resolution.  The caller must ensure that leftop and
 * rightop are suitable arguments for a cast operation; it's best to insert
 * parentheses if they aren't just variables or parameters.
 */
void
generate_operator_clause(StringInfo buf,
						 const char *leftop, Oid leftoptype,
						 Oid opoid,
						 const char *rightop, Oid rightoptype)
{
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", opoid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	Assert(operform->oprkind == 'b');
	oprname = NameStr(operform->oprname);

	nspname = get_namespace_name(operform->oprnamespace);

	appendStringInfoString(buf, leftop);
	if (leftoptype != operform->oprleft)
		add_cast_to(buf, operform->oprleft);
	appendStringInfo(buf, " OPERATOR(%s.", quote_identifier(nspname));
	appendStringInfoString(buf, oprname);
	appendStringInfo(buf, ") %s", rightop);
	if (rightoptype != operform->oprright)
		add_cast_to(buf, operform->oprright);

	ReleaseSysCache(opertup);
}

/*
 * Add a cast specification to buf.  We spell out the type name the hard way,
 * intentionally not using format_type_be().  This is to avoid corner cases
 * for CHARACTER, BIT, and perhaps other types, where specifying the type
 * using SQL-standard syntax results in undesirable data truncation.  By
 * doing it this way we can be certain that the cast will have default (-1)
 * target typmod.
 */
static void
add_cast_to(StringInfo buf, Oid typid)
{
	HeapTuple	typetup;
	Form_pg_type typform;
	char	   *typname;
	char	   *nspname;

	typetup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typetup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typform = (Form_pg_type) GETSTRUCT(typetup);

	typname = NameStr(typform->typname);
	nspname = get_namespace_name_or_temp(typform->typnamespace);

	appendStringInfo(buf, "::%s.%s",
					 quote_identifier(nspname), quote_identifier(typname));

	ReleaseSysCache(typetup);
}

/*
 * generate_qualified_type_name
 *		Compute the name to display for a type specified by OID
 *
 * This is different from format_type_be() in that we unconditionally
 * schema-qualify the name.  That also means no special syntax for
 * SQL-standard type names ... although in current usage, this should
 * only get used for domains, so such cases wouldn't occur anyway.
 */
static char *
generate_qualified_type_name(Oid typid)
{
	HeapTuple	tp;
	Form_pg_type typtup;
	char	   *typname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typtup = (Form_pg_type) GETSTRUCT(tp);
	typname = NameStr(typtup->typname);

	nspname = get_namespace_name_or_temp(typtup->typnamespace);
	if (!nspname)
		elog(ERROR, "cache lookup failed for namespace %u",
			 typtup->typnamespace);

	result = quote_qualified_identifier(nspname, typname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * generate_collation_name
 *		Compute the name to display for a collation specified by OID
 *
 * The result includes all necessary quoting and schema-prefixing.
 */
char *
generate_collation_name(Oid collid)
{
	HeapTuple	tp;
	Form_pg_collation colltup;
	char	   *collname;
	char	   *nspname;
	char	   *result;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for collation %u", collid);
	colltup = (Form_pg_collation) GETSTRUCT(tp);
	collname = NameStr(colltup->collname);

	if (!CollationIsVisible(collid))
		nspname = get_namespace_name_or_temp(colltup->collnamespace);
	else
		nspname = NULL;

	result = quote_qualified_identifier(nspname, collname);

	ReleaseSysCache(tp);

	return result;
}

/*
 * Given a C string, produce a TEXT datum.
 *
 * We assume that the input was palloc'd and may be freed.
 */
static text *
string_to_text(char *str)
{
	text	   *result;

	result = cstring_to_text(str);
	pfree(str);
	return result;
}

/*
 * Generate a C string representing a relation options from text[] datum.
 */
static void
get_reloptions(StringInfo buf, Datum reloptions)
{
	Datum	   *options;
	int			noptions;
	int			i;

	deconstruct_array_builtin(DatumGetArrayTypeP(reloptions), TEXTOID,
							  &options, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *option = TextDatumGetCString(options[i]);
		char	   *name;
		char	   *separator;
		char	   *value;

		/*
		 * Each array element should have the form name=value.  If the "=" is
		 * missing for some reason, treat it like an empty value.
		 */
		name = option;
		separator = strchr(option, '=');
		if (separator)
		{
			*separator = '\0';
			value = separator + 1;
		}
		else
			value = "";

		if (i > 0)
			appendStringInfoString(buf, ", ");
		appendStringInfo(buf, "%s=", quote_identifier(name));

		/*
		 * In general we need to quote the value; but to avoid unnecessary
		 * clutter, do not quote if it is an identifier that would not need
		 * quoting.  (We could also allow numbers, but that is a bit trickier
		 * than it looks --- for example, are leading zeroes significant?  We
		 * don't want to assume very much here about what custom reloptions
		 * might mean.)
		 */
		if (quote_identifier(value) == value)
			appendStringInfoString(buf, value);
		else
			simple_quote_literal(buf, value);

		pfree(option);
	}
}

/*
 * Generate a C string representing a relation's reloptions, or NULL if none.
 */
static char *
flatten_reloptions(Oid relid)
{
	char	   *result = NULL;
	HeapTuple	tuple;
	Datum		reloptions;
	bool		isnull;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	reloptions = SysCacheGetAttr(RELOID, tuple,
								 Anum_pg_class_reloptions, &isnull);
	if (!isnull)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		get_reloptions(&buf, reloptions);

		result = buf.data;
	}

	ReleaseSysCache(tuple);

	return result;
}

/*
 * get_range_partbound_string
 *		A C string representation of one range partition bound
 */
char *
get_range_partbound_string(List *bound_datums)
{
	deparse_context context;
	StringInfo	buf = makeStringInfo();
	ListCell   *cell;
	char	   *sep;

	memset(&context, 0, sizeof(deparse_context));
	context.buf = buf;

	appendStringInfoChar(buf, '(');
	sep = "";
	foreach(cell, bound_datums)
	{
		PartitionRangeDatum *datum =
			lfirst_node(PartitionRangeDatum, cell);

		appendStringInfoString(buf, sep);
		if (datum->kind == PARTITION_RANGE_DATUM_MINVALUE)
			appendStringInfoString(buf, "MINVALUE");
		else if (datum->kind == PARTITION_RANGE_DATUM_MAXVALUE)
			appendStringInfoString(buf, "MAXVALUE");
		else
		{
			Const	   *val = castNode(Const, datum->value);

			get_const_expr(val, &context, -1);
		}
		sep = ", ";
	}
	appendStringInfoChar(buf, ')');

	return buf->data;
}
