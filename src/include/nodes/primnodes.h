/*-------------------------------------------------------------------------
 *
 * primnodes.h
 *	"原始"节点类型定义:在查询流水线的解析/计划/执行
 *	阶段中不止一处使用那些。当前多为可执行表达式
 *	与连接树的节点。
 *	见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/primnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PRIMNODES_H
#define PRIMNODES_H

#include "access/attnum.h"
#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"


/* ----------------------------------------------------------------
 *	节点定义
 * ----------------------------------------------------------------
 */

/*
 * Alias -
 *	指定范围变量的别名;别名也可能指定表内列的
 *	重命名。
 *
 * 注:colnames 是 String 节点列表。与 RTE 关联 Alias
 * 结构中可能有对应已删列项;通常为空串("").细节见
 * parsenodes.h。
 */
typedef struct Alias
{
	NodeTag		type;
	char	   *aliasname;		/* aliased rel name (never qualified) */
	List	   *colnames;		/* 可选的列别名列表 */
} Alias;

/* 临时关系在提交时做什么 */
typedef enum OnCommitAction
{
	ONCOMMIT_NOOP,				/* No ON COMMIT clause (do nothing) */
	ONCOMMIT_PRESERVE_ROWS,		/* ON COMMIT PRESERVE ROWS (do nothing) */
	ONCOMMIT_DELETE_ROWS,		/* ON COMMIT DELETE ROWS */
	ONCOMMIT_DROP				/* ON COMMIT DROP */
} OnCommitAction;

/*
 * RangeVar - 范围变量,用于 FROM 子句
 *
 * 也用于表示 utility 语句中的表名;彼时不用 alias 字段,
 * inh 为否递归作供子表.某些上下文中也便于在此
 * 携带 TEMP 表指示。
 * 见上。
 */
typedef struct RangeVar
{
	NodeTag		type;

	/* 目录(数据库)名,或 NULL */
	char	   *catalogname;

	/* 模式名,或 NULL */
	char	   *schemaname;

	/* 关系/序列名 */
	char	   *relname;

	/* 按继承展开关系?递归作用于子? */
	bool		inh;

	/* see RELPERSISTENCE_* in pg_class.h */
	char		relpersistence;

	/* 表别名与可选列别名 */
	Alias	   *alias;

	/* 记号位置,未知则为 -1 */
	int			location;
} RangeVar;

/*
 * TableFunc - 表函数节点,如 XMLTABLE。
 *
 * ns_names 列表中项或为含字面命名空间名的 String
 * 节点,或为表示 DEFAULT 的 NULL 指针。
 */
typedef struct TableFunc
{
	NodeTag		type;
	/* 命名空间 URI 表达式列表 */
	List	   *ns_uris pg_node_attr(query_jumble_ignore);
	/* 命名空间名列表或 NULL */
	List	   *ns_names pg_node_attr(query_jumble_ignore);
	/* 输入文档表达式 */
	Node	   *docexpr;
	/* 行过滤表达式 */
	Node	   *rowexpr;
	/* 列名(String 列表) */
	List	   *colnames pg_node_attr(query_jumble_ignore);
	/* 列类型 OID 的 OID 列表 */
	List	   *coltypes pg_node_attr(query_jumble_ignore);
	/* 列 typmod 的整数列表 */
	List	   *coltypmods pg_node_attr(query_jumble_ignore);
	/* 列排序规则 OID  OID 列表 */
	List	   *colcollations pg_node_attr(query_jumble_ignore);
	/* 列过滤表达式列表 */
	List	   *colexprs;
	/* 列默认表达式列表 */
	List	   *coldefexprs pg_node_attr(query_jumble_ignore);
	/* 各输出列的可空标志 */
	Bitmapset  *notnulls pg_node_attr(query_jumble_ignore);
	/* 从 0 计;-1 表示未指定 */
	int			ordinalitycol pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} TableFunc;

/*
 * IntoClause - target information for SELECT INTO, CREATE TABLE AS, and
 * CREATE MATERIALIZED VIEW
 *
 * For CREATE MATERIALIZED VIEW, viewQuery is the parsed-but-not-rewritten
 * SELECT Query for the view; otherwise it's NULL.  This is irrelevant in
 * the query jumbling as CreateTableAsStmt already includes a reference to
 * its own Query, so ignore it.  (Although it's actually Query*, we declare
 * it as Node* to avoid a forward reference.)
 */
typedef struct IntoClause
{
	NodeTag		type;

	RangeVar   *rel;			/* 目标关系名 */
	List	   *colNames;		/* 要赋的列名,或 NIL */
	char	   *accessMethod;	/* 表访问方法 */
	List	   *options;		/* WITH 子句中的选项 */
	OnCommitAction onCommit;	/* COMMIT 时做什么? */
	char	   *tableSpaceName; /* 要用的表空间,或 NULL */
	/* 物化视图的 SELECT 查询 */
	Node	   *viewQuery pg_node_attr(query_jumble_ignore);
	bool		skipData;		/* true for WITH NO DATA */
} IntoClause;


/* ----------------------------------------------------------------
 *	可执行表达式的节点类型
 * ----------------------------------------------------------------
 */

/*
 * Expr - 可执行表达式节点的通用超类
 *
 * 可执行表达式树中使用的所有节点类型都应派生自 Expr
 * (即以 Expr 为第一字段)。因 Expr 仅含 NodeTag,这只是
 * 形式,但是一种简便文档方式.另见 execnodes.h 中的
 * ExprState 节点类型。
 */
typedef struct Expr
{
	pg_node_attr(abstract)

	NodeTag		type;
} Expr;

/*
 * Var - expression node representing a variable (ie, a table column)
 *
 * In the parser and planner, varno and varattno identify the semantic
 * referent, which is a base-relation column unless the reference is to a join
 * USING column that isn't semantically equivalent to either join input column
 * (because it is a FULL join or the input column requires a type coercion).
 * In those cases varno and varattno refer to the JOIN RTE.  (Early in the
 * planner, we replace such join references by the implied expression; but up
 * till then we want join reference Vars to keep their original identity for
 * query-printing purposes.)
 *
 * At the end of planning, Var nodes appearing in upper-level plan nodes are
 * reassigned to point to the outputs of their subplans; for example, in a
 * join node varno becomes INNER_VAR or OUTER_VAR and varattno becomes the
 * index of the proper element of that subplan's target list.  Similarly,
 * INDEX_VAR is used to identify Vars that reference an index column rather
 * than a heap column.  (In ForeignScan and CustomScan plan nodes, INDEX_VAR
 * is abused to signify references to columns of a custom scan tuple type.)
 *
 * ROWID_VAR is used in the planner to identify nonce variables that carry
 * row identity information during UPDATE/DELETE/MERGE.  This value should
 * never be seen outside the planner.
 *
 * varnullingrels is the set of RT indexes of outer joins that can force
 * the Var's value to null (at the point where it appears in the query).
 * See optimizer/README for discussion of that.
 *
 * varlevelsup is greater than zero in Vars that represent outer references.
 * Note that it affects the meaning of all of varno, varnullingrels, and
 * varnosyn, all of which refer to the range table of that query level.
 *
 * In the parser, varnosyn and varattnosyn are either identical to
 * varno/varattno, or they specify the column's position in an aliased JOIN
 * RTE that hides the semantic referent RTE's refname.  This is a syntactic
 * identifier as opposed to the semantic identifier; it tells ruleutils.c
 * how to print the Var properly.  varnosyn/varattnosyn retain their values
 * throughout planning and execution, so they are particularly helpful to
 * identify Vars when debugging.  Note, however, that a Var that is generated
 * in the planner and doesn't correspond to any simple relation column may
 * have varnosyn = varattnosyn = 0.
 */
#define    INNER_VAR		(-1)	/* 引用内子计划 */
#define    OUTER_VAR		(-2)	/* 引用外子计划 */
#define    INDEX_VAR		(-3)	/* 引用索引列 */
#define    ROWID_VAR		(-4)	/* 规划期的行标识列 */

#define IS_SPECIAL_VARNO(varno)		((int) (varno) < 0)

/* 规则中特殊 RTE 项下标的符号 */
#define    PRS2_OLD_VARNO			1
#define    PRS2_NEW_VARNO			2

typedef struct Var
{
	Expr		xpr;

	/*
	 * 本 var 关系在范围表中的下标,或
	 * INNER_VAR/OUTER_VAR/等
	 */
	int			varno;

	/*
	 * 本 var 的属性号,或零表示全部属性("整行 Var")
	 */
	AttrNumber	varattno;

	/* 本 var 类型的 pg_type OID */
	Oid			vartype pg_node_attr(query_jumble_ignore);
	/* pg_attribute typmod 值 */
	int32		vartypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			varcollid pg_node_attr(query_jumble_ignore);

	/*
	 * 可把 Var 值替换为空的外连接 RT 下标。
	 * 查询混淆中可省略 varnullingrels,因其完全由
	 * varno/varlevelsup 加 Var 的查询位置决定。
	 */
	Bitmapset  *varnullingrels pg_node_attr(query_jumble_ignore);

	/*
	 * 引用外层关系的子查询变量;普通 var 为 0,
	 * >0 为向上 N 层
	 */
	Index		varlevelsup;

	/*
	 * 相等性比较忽略 varnosyn/varattnosyn,因只要
	 * varno/varattno 匹配,不同语法标识符的 Vars 语义相同。
	 * 见上。
	 */
	/* 语法关系下标(未知则为 0) */
	Index		varnosyn pg_node_attr(equal_ignore, query_jumble_ignore);
	/* 语法属性号 */
	AttrNumber	varattnosyn pg_node_attr(equal_ignore, query_jumble_ignore);

	/* 记号位置,未知则为 -1 */
	int			location;
} Var;

/*
 * Const
 *
 * 注:对 varlena 数据类型,我们规定 Const 节点的值必须是
 * 非扩展形式(4 字节头,无压缩或外部引用)。这确保 Const
 * 自包含,并更可能让 equal() 把逻辑相同的值视为相等。
 * 见上。
 *
 * 查询混淆仅关心常量类型 OID。
 */
typedef struct Const
{
	pg_node_attr(custom_copy_equal, custom_read_write)

	Expr		xpr;
	/* 常量数据类型的 pg_type OID */
	Oid			consttype;
	/* typmod 值(若有) */
	int32		consttypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			constcollid pg_node_attr(query_jumble_ignore);
	/* 常量数据类型的 typlen */
	int			constlen pg_node_attr(query_jumble_ignore);
	/* 常量的值 */
	Datum		constvalue pg_node_attr(query_jumble_ignore);
	/* 常量是否为空(真则 constvalue 未定义) */
	bool		constisnull pg_node_attr(query_jumble_ignore);

	/*
	 * Whether this datatype is passed by value.  If true, then all the
	 * information is stored in the Datum.  If false, then the Datum contains
	 * a pointer to the information.
	 */
	bool		constbyval pg_node_attr(query_jumble_ignore);

	/*
	 * token location, or -1 if unknown.  All constants are tracked as
	 * locations in query jumbling, to be marked as parameters.
	 */
	int			location pg_node_attr(query_jumble_location);
} Const;

/*
 * Param
 *
 *		paramkind specifies the kind of parameter. The possible values
 *		for this field are:
 *
 *		PARAM_EXTERN:  The parameter value is supplied from outside the plan.
 *				Such parameters are numbered from 1 to n.
 *
 *		PARAM_EXEC:  The parameter is an internal executor parameter, used
 *				for passing values into and out of sub-queries or from
 *				nestloop joins to their inner scans.
 *				For historical reasons, such parameters are numbered from 0.
 *				These numbers are independent of PARAM_EXTERN numbers.
 *
 *		PARAM_SUBLINK:	The parameter represents an output column of a SubLink
 *				node's sub-select.  The column number is contained in the
 *				`paramid' field.  (This type of Param is converted to
 *				PARAM_EXEC during planning.)
 *
 *		PARAM_MULTIEXPR:  Like PARAM_SUBLINK, the parameter represents an
 *				output column of a SubLink node's sub-select, but here, the
 *				SubLink is always a MULTIEXPR SubLink.  The high-order 16 bits
 *				of the `paramid' field contain the SubLink's subLinkId, and
 *				the low-order 16 bits contain the column number.  (This type
 *				of Param is also converted to PARAM_EXEC during planning.)
 */
typedef enum ParamKind
{
	PARAM_EXTERN,
	PARAM_EXEC,
	PARAM_SUBLINK,
	PARAM_MULTIEXPR
} ParamKind;

typedef struct Param
{
	Expr		xpr;
	ParamKind	paramkind;		/* 参数种类。见上 */
	int			paramid;		/* 参数的数值 ID */
	Oid			paramtype;		/* 参数数据类型的 pg_type OID */
	/* typmod 值(若已知) */
	int32		paramtypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			paramcollid pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} Param;

/*
 * Aggref
 *
 * The aggregate's args list is a targetlist, ie, a list of TargetEntry nodes.
 *
 * For a normal (non-ordered-set) aggregate, the non-resjunk TargetEntries
 * represent the aggregate's regular arguments (if any) and resjunk TLEs can
 * be added at the end to represent ORDER BY expressions that are not also
 * arguments.  As in a top-level Query, the TLEs can be marked with
 * ressortgroupref indexes to let them be referenced by SortGroupClause
 * entries in the aggorder and/or aggdistinct lists.  This represents ORDER BY
 * and DISTINCT operations to be applied to the aggregate input rows before
 * they are passed to the transition function.  The grammar only allows a
 * simple "DISTINCT" specifier for the arguments, but we use the full
 * query-level representation to allow more code sharing.
 *
 * For an ordered-set aggregate, the args list represents the WITHIN GROUP
 * (aggregated) arguments, all of which will be listed in the aggorder list.
 * DISTINCT is not supported in this case, so aggdistinct will be NIL.
 * The direct arguments appear in aggdirectargs (as a list of plain
 * expressions, not TargetEntry nodes).
 *
 * aggtranstype is the data type of the state transition values for this
 * aggregate (resolved to an actual type, if agg's transtype is polymorphic).
 * This is determined during planning and is InvalidOid before that.
 *
 * aggargtypes is an OID list of the data types of the direct and regular
 * arguments.  Normally it's redundant with the aggdirectargs and args lists,
 * but in a combining aggregate, it's not because the args list has been
 * replaced with a single argument representing the partial-aggregate
 * transition values.
 *
 * aggpresorted is set by the query planner for ORDER BY and DISTINCT
 * aggregates where the chosen plan provides presorted input for this
 * aggregate during execution.
 *
 * aggsplit indicates the expected partial-aggregation mode for the Aggref's
 * parent plan node.  It's always set to AGGSPLIT_SIMPLE in the parser, but
 * the planner might change it to something else.  We use this mainly as
 * a crosscheck that the Aggrefs match the plan; but note that when aggsplit
 * indicates a non-final mode, aggtype reflects the transition data type
 * not the SQL-level output type of the aggregate.
 *
 * aggno and aggtransno are -1 in the parse stage, and are set in planning.
 * Aggregates with the same 'aggno' represent the same aggregate expression,
 * and can share the result.  Aggregates with same 'transno' but different
 * 'aggno' can share the same transition state, only the final function needs
 * to be called separately.
 *
 * Information related to collations, transition types and internal states
 * are irrelevant for the query jumbling.
 */
typedef struct Aggref
{
	Expr		xpr;

	/* 聚合的 pg_proc Oid */
	Oid			aggfnoid;

	/* 聚合结果的类型 Oid */
	Oid			aggtype pg_node_attr(query_jumble_ignore);

	/* 结果排序规则的 OID */
	Oid			aggcollid pg_node_attr(query_jumble_ignore);

	/* 函数应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/*
	 * 聚合转移值的类型 Oid;因可能尚未设置,equal 忽略
	 * 见上。
	 */
	Oid			aggtranstype pg_node_attr(equal_ignore, query_jumble_ignore);

	/* 直接与被聚合参数的类型 Oid */
	List	   *aggargtypes pg_node_attr(query_jumble_ignore);

	/* 直接参数(若为有序集聚合) */
	List	   *aggdirectargs;

	/* 被聚合参数与排序表达式 */
	List	   *args;

	/* ORDER BY(SortGroupClause 列表) */
	List	   *aggorder;

	/* DISTINCT(SortGroupClause 列表) */
	List	   *aggdistinct;

	/* FILTER 表达式(若有) */
	Expr	   *aggfilter;

	/* 若参数列表实际为 * 则为真 */
	bool		aggstar pg_node_attr(query_jumble_ignore);

	/*
	 * 变长参数已合并为数组末参数则为真
	 * 见上.
	 */
	bool		aggvariadic pg_node_attr(query_jumble_ignore);

	/* 聚合种类(见 pg_aggregate.h) */
	char		aggkind pg_node_attr(query_jumble_ignore);

	/* 聚合输入已排序 */
	bool		aggpresorted pg_node_attr(equal_ignore, query_jumble_ignore);

	/* 若聚合属于外查询则 >0 */
	Index		agglevelsup pg_node_attr(query_jumble_ignore);

	/* 父 Agg 的预期聚合拆分模式 */
	AggSplit	aggsplit pg_node_attr(query_jumble_ignore);

	/* Agg 节点内的唯一 ID */
	int			aggno pg_node_attr(query_jumble_ignore);

	/* Agg 中转移状态的唯一 ID */
	int			aggtransno pg_node_attr(query_jumble_ignore);

	/* 记号位置,未知则为 -1 */
	int			location;
} Aggref;

/*
 * GroupingFunc
 *
 * A GroupingFunc is a GROUPING(...) expression, which behaves in many ways
 * like an aggregate function (e.g. it "belongs" to a specific query level,
 * which might not be the one immediately containing it), but also differs in
 * an important respect: it never evaluates its arguments, they merely
 * designate expressions from the GROUP BY clause of the query level to which
 * it belongs.
 *
 * The spec defines the evaluation of GROUPING() purely by syntactic
 * replacement, but we make it a real expression for optimization purposes so
 * that one Agg node can handle multiple grouping sets at once.  Evaluating the
 * result only needs the column positions to check against the grouping set
 * being projected.  However, for EXPLAIN to produce meaningful output, we have
 * to keep the original expressions around, since expression deparse does not
 * give us any feasible way to get at the GROUP BY clause.
 *
 * Also, we treat two GroupingFunc nodes as equal if they have equal arguments
 * lists and agglevelsup, without comparing the refs and cols annotations.
 *
 * In raw parse output we have only the args list; parse analysis fills in the
 * refs list, and the planner fills in the cols list.
 *
 * All the fields used as information for an internal state are irrelevant
 * for the query jumbling.
 */
typedef struct GroupingFunc
{
	Expr		xpr;

	/* 参数,不求值但为 EXPLAIN 等保留 */
	List	   *args pg_node_attr(query_jumble_ignore);

	/* 参数的 ressortgrouprefs */
	List	   *refs pg_node_attr(equal_ignore);

	/* 规划器设置的实际列位置 */
	List	   *cols pg_node_attr(equal_ignore, query_jumble_ignore);

	/* same as Aggref.agglevelsup */
	Index		agglevelsup;

	/* 记号位置 */
	int			location;
} GroupingFunc;

/*
 * WindowFunc
 *
 * 排序规则信息与查询混淆无关,节点内部状态如 winstar
 * 与 winagg 亦然。
 */
typedef struct WindowFunc
{
	Expr		xpr;
	/* 函数的 pg_proc Oid */
	Oid			winfnoid;
	/* 窗口函数结果的类型 Oid */
	Oid			wintype pg_node_attr(query_jumble_ignore);
	/* 结果排序规则的 OID */
	Oid			wincollid pg_node_attr(query_jumble_ignore);
	/* 函数应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* 窗口函数的参数 */
	List	   *args;
	/* FILTER 表达式(若有) */
	Expr	   *aggfilter;
	/* 关联 WindowClause 的下标 */
	Index		winref;
	/* 若参数列表实际为 * 则为真 */
	bool		winstar pg_node_attr(query_jumble_ignore);
	/* 函数是否为简单聚合? */
	bool		winagg pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} WindowFunc;

/*
 * SubscriptingRef: describes a subscripting operation over a container
 * (array, etc).
 *
 * A SubscriptingRef can describe fetching a single element from a container,
 * fetching a part of a container (e.g. an array slice), storing a single
 * element into a container, or storing a slice.  The "store" cases work with
 * an initial container value and a source value that is inserted into the
 * appropriate part of the container; the result of the operation is an
 * entire new modified container value.
 *
 * If reflowerindexpr = NIL, then we are fetching or storing a single container
 * element at the subscripts given by refupperindexpr. Otherwise we are
 * fetching or storing a container slice, that is a rectangular subcontainer
 * with lower and upper bounds given by the index expressions.
 * reflowerindexpr must be the same length as refupperindexpr when it
 * is not NIL.
 *
 * In the slice case, individual expressions in the subscript lists can be
 * NULL, meaning "substitute the array's current lower or upper bound".
 * (Non-array containers may or may not support this.)
 *
 * refcontainertype is the actual container type that determines the
 * subscripting semantics.  (This will generally be either the exposed type of
 * refexpr, or the base type if that is a domain.)  refelemtype is the type of
 * the container's elements; this is saved for the use of the subscripting
 * functions, but is not used by the core code.  refrestype, reftypmod, and
 * refcollid describe the type of the SubscriptingRef's result.  In a store
 * expression, refrestype will always match refcontainertype; in a fetch,
 * it could be refelemtype for an element fetch, or refcontainertype for a
 * slice fetch, or possibly something else as determined by type-specific
 * subscripting logic.  Likewise, reftypmod and refcollid will match the
 * container's properties in a store, but could be different in a fetch.
 *
 * Any internal state data is ignored for the query jumbling.
 *
 * Note: for the cases where a container is returned, if refexpr yields a R/W
 * expanded container, then the implementation is allowed to modify that
 * object in-place and return the same object.
 */
typedef struct SubscriptingRef
{
	Expr		xpr;
	/* 容器本身的类型 */
	Oid			refcontainertype pg_node_attr(query_jumble_ignore);
	/* 容器类型的 pg_type.typelem */
	Oid			refelemtype pg_node_attr(query_jumble_ignore);
	/* SubscriptingRef 结果的类型 */
	Oid			refrestype pg_node_attr(query_jumble_ignore);
	/* 结果的 typmod */
	int32		reftypmod pg_node_attr(query_jumble_ignore);
	/* 结果排序规则,无则为 InvalidOid */
	Oid			refcollid pg_node_attr(query_jumble_ignore);
	/* 求值为容器上界下标的表达式 */
	List	   *refupperindexpr;

	/*
	 * 求值为容器下界下标表达式,或 NIL 表示单容器元素。
	 * 见上。
	 */
	List	   *reflowerindexpr;
	/* 求值为容器值的表达式 */
	Expr	   *refexpr;
	/* 源值表达式,若为取则为 NULL */
	Expr	   *refassgnexpr;
} SubscriptingRef;

/*
 * CoercionContext - 区分允许的类型强制集合
 *
 * 注:备选项次序有意义;较大值比较小值允许更多强制.
 * 见上。
 */
typedef enum CoercionContext
{
	COERCION_IMPLICIT,			/* 表达式上下文中的强制 */
	COERCION_ASSIGNMENT,		/* 赋值上下文中的强制 */
	COERCION_PLPGSQL,			/* 若无赋值强制则用 CoerceViaIO */
	COERCION_EXPLICIT			/* 显式强制操作 */
} CoercionContext;

/*
 * CoercionForm - how to display a FuncExpr or related node
 *
 * "Coercion" is a bit of a misnomer, since this value records other
 * special syntaxes besides casts, but for now we'll keep this naming.
 *
 * NB: equal() ignores CoercionForm fields, therefore this *must* not carry
 * any semantically significant information.  We need that behavior so that
 * the planner will consider equivalent implicit and explicit casts to be
 * equivalent.  In cases where those actually behave differently, the coercion
 * function's arguments will be different.
 */
typedef enum CoercionForm
{
	COERCE_EXPLICIT_CALL,		/* 显示为函数调用 */
	COERCE_EXPLICIT_CAST,		/* 显示为显式强制 */
	COERCE_IMPLICIT_CAST,		/* 隐式强制,故隐藏 */
	COERCE_SQL_SYNTAX			/* 用 SQL 规定的特殊语法显示 */
} CoercionForm;

/*
 * FuncExpr - expression node for a function call
 *
 * Collation information is irrelevant for the query jumbling, only the
 * arguments and the function OID matter.
 */
typedef struct FuncExpr
{
	Expr		xpr;
	/* 函数的 PG_PROC OID */
	Oid			funcid;
	/* 结果值的 PG_TYPE OID */
	Oid			funcresulttype pg_node_attr(query_jumble_ignore);
	/* 函数是否返回集合 */
	bool		funcretset pg_node_attr(query_jumble_ignore);

	/*
	 * 变长参数已合并为数组末参数则为真
	 * 见上.
	 */
	bool		funcvariadic pg_node_attr(query_jumble_ignore);
	/* 如何显示此函数调用 */
	CoercionForm funcformat pg_node_attr(query_jumble_ignore);
	/* 结果排序规则的 OID */
	Oid			funccollid pg_node_attr(query_jumble_ignore);
	/* 函数应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* 函数的参数 */
	List	   *args;
	/* 记号位置,未知则为 -1 */
	int			location;
} FuncExpr;

/*
 * NamedArgExpr - a named argument of a function
 *
 * This node type can only appear in the args list of a FuncCall or FuncExpr
 * node.  We support pure positional call notation (no named arguments),
 * named notation (all arguments are named), and mixed notation (unnamed
 * arguments followed by named ones).
 *
 * Parse analysis sets argnumber to the positional index of the argument,
 * but doesn't rearrange the argument list.
 *
 * The planner will convert argument lists to pure positional notation
 * during expression preprocessing, so execution never sees a NamedArgExpr.
 */
typedef struct NamedArgExpr
{
	Expr		xpr;
	/* 参数表达式 */
	Expr	   *arg;
	/* 名称 */
	char	   *name pg_node_attr(query_jumble_ignore);
	/* 位置记法中的参数编号 */
	int			argnumber;
	/* 参数名位置,未知则为 -1 */
	int			location;
} NamedArgExpr;

/*
 * OpExpr - expression node for an operator invocation
 *
 * Semantically, this is essentially the same as a function call.
 *
 * Note that opfuncid is not necessarily filled in immediately on creation
 * of the node.  The planner makes sure it is valid before passing the node
 * tree to the executor, but during parsing/planning opfuncid can be 0.
 * Therefore, equal() will accept a zero value as being equal to other values.
 *
 * Internal state information and collation data is irrelevant for the query
 * jumbling.
 */
typedef struct OpExpr
{
	Expr		xpr;

	/* 运算符的 PG_OPERATOR OID */
	Oid			opno;

	/* 底层函数的 PG_PROC OID */
	Oid			opfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* 结果值的 PG_TYPE OID */
	Oid			opresulttype pg_node_attr(query_jumble_ignore);

	/* 运算符是否返回集合 */
	bool		opretset pg_node_attr(query_jumble_ignore);

	/* 结果排序规则的 OID */
	Oid			opcollid pg_node_attr(query_jumble_ignore);

	/* 运算符应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/* 运算符的参数(1 或 2 个) */
	List	   *args;

	/* 记号位置,未知则为 -1 */
	int			location;
} OpExpr;

/*
 * DistinctExpr - expression node for "x IS DISTINCT FROM y"
 *
 * Except for the nodetag, this is represented identically to an OpExpr
 * referencing the "=" operator for x and y.
 * We use "=", not the more obvious "<>", because more datatypes have "="
 * than "<>".  This means the executor must invert the operator result.
 * Note that the operator function won't be called at all if either input
 * is NULL, since then the result can be determined directly.
 */
typedef OpExpr DistinctExpr;

/*
 * NullIfExpr - NULLIF 表达式
 *
 * 与 DistinctExpr 类似,表示法与引用 x、y 的 "=" 运算符的
 * OpExpr 相同。
 */
typedef OpExpr NullIfExpr;

/*
 * ScalarArrayOpExpr - expression node for "scalar op ANY/ALL (array)"
 *
 * The operator must yield boolean.  It is applied to the left operand
 * and each element of the righthand array, and the results are combined
 * with OR or AND (for ANY or ALL respectively).  The node representation
 * is almost the same as for the underlying operator, but we need a useOr
 * flag to remember whether it's ANY or ALL, and we don't have to store
 * the result type (or the collation) because it must be boolean.
 *
 * A ScalarArrayOpExpr with a valid hashfuncid is evaluated during execution
 * by building a hash table containing the Const values from the RHS arg.
 * This table is probed during expression evaluation.  The planner will set
 * hashfuncid to the hash function which must be used to build and probe the
 * hash table.  The executor determines if it should use hash-based checks or
 * the more traditional means based on if the hashfuncid is set or not.
 *
 * When performing hashed NOT IN, the negfuncid will also be set to the
 * equality function which the hash table must use to build and probe the hash
 * table.  opno and opfuncid will remain set to the <> operator and its
 * corresponding function and won't be used during execution.  For
 * non-hashtable based NOT INs, negfuncid will be set to InvalidOid.  See
 * convert_saop_to_hashed_saop().
 *
 * Similar to OpExpr, opfuncid, hashfuncid, and negfuncid are not necessarily
 * filled in right away, so will be ignored for equality if they are not set
 * yet.
 *
 * OID entries of the internal function types are irrelevant for the query
 * jumbling, but the operator OID and the arguments are.
 */
typedef struct ScalarArrayOpExpr
{
	Expr		xpr;

	/* 运算符的 PG_OPERATOR OID */
	Oid			opno;

	/* 比较函数的 PG_PROC OID */
	Oid			opfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* 哈希函数 PG_PROC OID 或InvalidOid */
	Oid			hashfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* opfuncid 函数否定者的 PG_PROC OID 或 InvalidOid。见上 */
	Oid			negfuncid pg_node_attr(equal_ignore_if_zero, query_jumble_ignore);

	/* ANY 为真,ALL 为假 */
	bool		useOr;

	/* 运算符应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);

	/* 标量与数组操作数 */
	List	   *args;

	/* 记号位置,未知则为 -1 */
	int			location;
} ScalarArrayOpExpr;

/*
 * BoolExpr - 基本布尔运算符 AND、OR、NOT 的表达式节点
 *
 * 注意参数以 List 给出.对 NOT 列表当然恰有一个元素。
 * 对 AND 与 OR 可有两个或更多参数。
 * 见上。
 */
typedef enum BoolExprType
{
	AND_EXPR, OR_EXPR, NOT_EXPR
} BoolExprType;

typedef struct BoolExpr
{
	pg_node_attr(custom_read_write)

	Expr		xpr;
	BoolExprType boolop;
	List	   *args;			/* 本表达式的参数 */
	int			location;		/* 记号位置,未知则为 -1 */
} BoolExpr;

/*
 * SubLink
 *
 * A SubLink represents a subselect appearing in an expression, and in some
 * cases also the combining operator(s) just above it.  The subLinkType
 * indicates the form of the expression represented:
 *	EXISTS_SUBLINK		EXISTS(SELECT ...)
 *	ALL_SUBLINK			(lefthand) op ALL (SELECT ...)
 *	ANY_SUBLINK			(lefthand) op ANY (SELECT ...)
 *	ROWCOMPARE_SUBLINK	(lefthand) op (SELECT ...)
 *	EXPR_SUBLINK		(SELECT with single targetlist item ...)
 *	MULTIEXPR_SUBLINK	(SELECT with multiple targetlist items ...)
 *	ARRAY_SUBLINK		ARRAY(SELECT with single targetlist item ...)
 *	CTE_SUBLINK			WITH query (never actually part of an expression)
 * For ALL, ANY, and ROWCOMPARE, the lefthand is a list of expressions of the
 * same length as the subselect's targetlist.  ROWCOMPARE will *always* have
 * a list with more than one entry; if the subselect has just one target
 * then the parser will create an EXPR_SUBLINK instead (and any operator
 * above the subselect will be represented separately).
 * ROWCOMPARE, EXPR, and MULTIEXPR require the subselect to deliver at most
 * one row (if it returns no rows, the result is NULL).
 * ALL, ANY, and ROWCOMPARE require the combining operators to deliver boolean
 * results.  ALL and ANY combine the per-row results using AND and OR
 * semantics respectively.
 * ARRAY requires just one target column, and creates an array of the target
 * column's type using any number of rows resulting from the subselect.
 *
 * SubLink is classed as an Expr node, but it is not actually executable;
 * it must be replaced in the expression tree by a SubPlan node during
 * planning.
 *
 * NOTE: in the raw output of gram.y, testexpr contains just the raw form
 * of the lefthand expression (if any), and operName is the String name of
 * the combining operator.  Also, subselect is a raw parsetree.  During parse
 * analysis, the parser transforms testexpr into a complete boolean expression
 * that compares the lefthand value(s) to PARAM_SUBLINK nodes representing the
 * output columns of the subselect.  And subselect is transformed to a Query.
 * This is the representation seen in saved rules and in the rewriter.
 *
 * In EXISTS, EXPR, MULTIEXPR, and ARRAY SubLinks, testexpr and operName
 * are unused and are always null.
 *
 * subLinkId is currently used only for MULTIEXPR SubLinks, and is zero in
 * other SubLinks.  This number identifies different multiple-assignment
 * subqueries within an UPDATE statement's SET list.  It is unique only
 * within a particular targetlist.  The output column(s) of the MULTIEXPR
 * are referenced by PARAM_MULTIEXPR Params appearing elsewhere in the tlist.
 *
 * The CTE_SUBLINK case never occurs in actual SubLink nodes, but it is used
 * in SubPlans generated for WITH subqueries.
 */
typedef enum SubLinkType
{
	EXISTS_SUBLINK,
	ALL_SUBLINK,
	ANY_SUBLINK,
	ROWCOMPARE_SUBLINK,
	EXPR_SUBLINK,
	MULTIEXPR_SUBLINK,
	ARRAY_SUBLINK,
	CTE_SUBLINK					/* 仅供 SubPlans */
} SubLinkType;


typedef struct SubLink
{
	Expr		xpr;
	SubLinkType subLinkType;	/* 见上 */
	int			subLinkId;		/* ID (1..n); 0 if not MULTIEXPR */
	Node	   *testexpr;		/* ALL/ANY/ROWCOMPARE 的外查询测试 */
	/* 原先指定的运算符名 */
	List	   *operName pg_node_attr(query_jumble_ignore);
	/* subselect as Query* or raw parsetree */
	Node	   *subselect;
	int			location;		/* 记号位置,未知则为 -1 */
} SubLink;

/*
 * SubPlan - executable expression node for a subplan (sub-SELECT)
 *
 * The planner replaces SubLink nodes in expression trees with SubPlan
 * nodes after it has finished planning the subquery.  SubPlan references
 * a sub-plantree stored in the subplans list of the toplevel PlannedStmt.
 * (We avoid a direct link to make it easier to copy expression trees
 * without causing multiple processing of the subplan.)
 *
 * In an ordinary subplan, testexpr points to an executable expression
 * (OpExpr, an AND/OR tree of OpExprs, or RowCompareExpr) for the combining
 * operator(s); the left-hand arguments are the original lefthand expressions,
 * and the right-hand arguments are PARAM_EXEC Param nodes representing the
 * outputs of the sub-select.  (NOTE: runtime coercion functions may be
 * inserted as well.)  This is just the same expression tree as testexpr in
 * the original SubLink node, but the PARAM_SUBLINK nodes are replaced by
 * suitably numbered PARAM_EXEC nodes.
 *
 * If the sub-select becomes an initplan rather than a subplan, the executable
 * expression is part of the outer plan's expression tree (and the SubPlan
 * node itself is not, but rather is found in the outer plan's initPlan
 * list).  In this case testexpr is NULL to avoid duplication.
 *
 * The planner also derives lists of the values that need to be passed into
 * and out of the subplan.  Input values are represented as a list "args" of
 * expressions to be evaluated in the outer-query context (currently these
 * args are always just Vars, but in principle they could be any expression).
 * The values are assigned to the global PARAM_EXEC params indexed by parParam
 * (the parParam and args lists must have the same ordering).  setParam is a
 * list of the PARAM_EXEC params that are computed by the sub-select, if it
 * is an initplan or MULTIEXPR plan; they are listed in order by sub-select
 * output column position.  (parParam and setParam are integer Lists, not
 * Bitmapsets, because their ordering is significant.)
 *
 * Also, the planner computes startup and per-call costs for use of the
 * SubPlan.  Note that these include the cost of the subquery proper,
 * evaluation of the testexpr if any, and any hashtable management overhead.
 */
typedef struct SubPlan
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;
	/* 自原 SubLink 拷贝的字段: */
	SubLinkType subLinkType;	/* 见上 */
	/* 组合运算符,已变为可执行表达式: */
	Node	   *testexpr;		/* OpExpr 或 RowCompareExpr 表达式树 */
	List	   *paramIds;		/* 嵌入上述的 Params 的 ID */
	/* 要用的 Plan 树的标识: */
	int			plan_id;		/* PlannedStmt.subplans 中下标(从 1) */
	/* 供 EXPLAIN 与调试的 SubPlan 标识: */
	char	   *plan_name;		/* 规划期赋的名称 */
	/* 确定子计划输出类型的额外数据: */
	Oid			firstColType;	/* 子计划结果第一列的类型 */
	int32		firstColTypmod; /* 子计划结果第一列的 typmod */
	Oid			firstColCollation;	/* Collation of first column of subplan
									 * result */
	/* 关于执行策略的信息: */
	bool		useHashTable;	/* 真则把子选择输出存入哈希
								 * 表(意指在做 "IN") */
	bool		unknownEqFalse; /* 规范结果为 UNKNOWN 时返回 FALSE
								 * 也可;这使空值处理简单得多
								 * 见上。 */
	bool		parallel_safe;	/* 子计划是否并行安全? */
	/* 注:parallel_safe 不考虑 testexpr 或 args 的内容 */
	/* 向子选择传入传出参数的信息: */
	/* setParam 与 parParam 是整数列表(参数 ID) */
	List	   *setParam;		/* initplan and MULTIEXPR subqueries have to
								 * set these Params for parent plan */
	List	   *parParam;		/* 来自父计划的输入 Params 下标 */
	List	   *args;			/* exprs to pass as parParam values */
	/* 估计的执行代价: */
	Cost		startup_cost;	/* one-time setup cost */
	Cost		per_call_cost;	/* 每次子计划求值的代价 */
} SubPlan;

/*
 * AlternativeSubPlan - expression node for a choice among SubPlans
 *
 * This is used only transiently during planning: by the time the plan
 * reaches the executor, all AlternativeSubPlan nodes have been removed.
 *
 * The subplans are given as a List so that the node definition need not
 * change if there's ever more than two alternatives.  For the moment,
 * though, there are always exactly two; and the first one is the fast-start
 * plan.
 */
typedef struct AlternativeSubPlan
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;
	List	   *subplans;		/* 结果等价的 SubPlan */
} AlternativeSubPlan;

/* ----------------
 * FieldSelect
 *
 * FieldSelect represents the operation of extracting one field from a tuple
 * value.  At runtime, the input expression is expected to yield a rowtype
 * Datum.  The specified field number is extracted and returned as a Datum.
 * ----------------
 */

typedef struct FieldSelect
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	AttrNumber	fieldnum;		/* 要提取的字段属性号 */
	/* 字段类型(本节点的结果类型) */
	Oid			resulttype pg_node_attr(query_jumble_ignore);
	/* output typmod (usually -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* 字段排序规则的 OID */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
} FieldSelect;

/* ----------------
 * FieldStore
 *
 * FieldStore 为修改元组值中一个字段,产出新元组值操作
 * (不改动输入!).类似 SubscriptingRef 赋值情形,用于实现
 * 对列的一部分做 UPDATE。
 * 见上。
 *
 * resulttype 恒为命名复合类型(非域).要更新复合域值,对
 * FieldStore 应用 CoerceToDomain。
 *
 * 单个 FieldStore 实际可表示对多个不同字段更新.解析器
 * 只生成单元素列表 FieldStore,但规划器会把对同一基列的
 * 多次更新折叠为一个 FieldStore。
 * 见上。
 * ----------------
 */

typedef struct FieldStore
{
	Expr		xpr;
	Expr	   *arg;			/* 输入元组值 */
	List	   *newvals;		/* 字段的新值 */
	/* 字段 attnums 整数列表 */
	List	   *fieldnums pg_node_attr(query_jumble_ignore);
	/* 结果类型(与 arg 类型相同) */
	Oid			resulttype pg_node_attr(query_jumble_ignore);
	/* 与 RowExpr 一样,故意省略 typmod 与排序规则 */
} FieldStore;

/* ----------------
 * RelabelType
 *
 * RelabelType represents a "dummy" type coercion between two binary-
 * compatible datatypes, such as reinterpreting the result of an OID
 * expression as an int4.  It is a no-op at runtime; we only need it
 * to provide a place to store the correct type to be attributed to
 * the expression result during type resolution.  (We can't get away
 * with just overwriting the type field of the input expression node,
 * so we need a separate node to show the coercion's result type.)
 * ----------------
 */

typedef struct RelabelType
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	Oid			resulttype;		/* 强制表达式的输出类型 */
	/* output typmod (usually -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* 如何显示本节点 */
	CoercionForm relabelformat pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} RelabelType;

/* ----------------
 * CoerceViaIO
 *
 * CoerceViaIO 表示文本表示兼容两类型间的强制,通过调用
 * 源类型的 typoutput 再调用目标类型的 typinput 实现。
 * 见上。
 * ----------------
 */

typedef struct CoerceViaIO
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	Oid			resulttype;		/* 强制的输出类型 */
	/* 不存输出 typmod,假定为 -1 */
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* 如何显示本节点 */
	CoercionForm coerceformat pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} CoerceViaIO;

/* ----------------
 * ArrayCoerceExpr
 *
 * ArrayCoerceExpr represents a type coercion from one array type to another,
 * which is implemented by applying the per-element coercion expression
 * "elemexpr" to each element of the source array.  Within elemexpr, the
 * source element is represented by a CaseTestExpr node.  Note that even if
 * elemexpr is a no-op (that is, just CaseTestExpr + RelabelType), the
 * coercion still requires some effort: we have to fix the element type OID
 * stored in the array header.
 * ----------------
 */

typedef struct ArrayCoerceExpr
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式(产出数组) */
	Expr	   *elemexpr;		/* 表示逐元素工作的表达式 */
	Oid			resulttype;		/* 强制的输出类型(数组类型) */
	/* 输出 typmod(亦即元素 typmod) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* 如何显示本节点 */
	CoercionForm coerceformat pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} ArrayCoerceExpr;

/* ----------------
 * ConvertRowtypeExpr
 *
 * ConvertRowtypeExpr represents a type coercion from one composite type
 * to another, where the source type is guaranteed to contain all the columns
 * needed for the destination type plus possibly others; the columns need not
 * be in the same positions, but are matched up by name.  This is primarily
 * used to convert a whole-row value of an inheritance child table into a
 * valid whole-row value of its parent table's rowtype.  Both resulttype
 * and the exposed type of "arg" must be named composite types (not domains).
 * ----------------
 */

typedef struct ConvertRowtypeExpr
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	Oid			resulttype;		/* 输出类型(恒为复合类型) */
	/* 与 RowExpr 一样,故意省略 typmod 与排序规则 */
	/* 如何显示本节点 */
	CoercionForm convertformat pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} ConvertRowtypeExpr;

/*----------
 * CollateExpr - COLLATE
 *
 * The planner replaces CollateExpr with RelabelType during expression
 * preprocessing, so execution never sees a CollateExpr.
 *----------
 */
typedef struct CollateExpr
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	Oid			collOid;		/* collation's OID */
	int			location;		/* 记号位置,未知则为 -1 */
} CollateExpr;

/*----------
 * CaseExpr - a CASE expression
 *
 * We support two distinct forms of CASE expression:
 *		CASE WHEN boolexpr THEN expr [ WHEN boolexpr THEN expr ... ]
 *		CASE testexpr WHEN compexpr THEN expr [ WHEN compexpr THEN expr ... ]
 * These are distinguishable by the "arg" field being NULL in the first case
 * and the testexpr in the second case.
 *
 * In the raw grammar output for the second form, the condition expressions
 * of the WHEN clauses are just the comparison values.  Parse analysis
 * converts these to valid boolean expressions of the form
 *		CaseTestExpr '=' compexpr
 * where the CaseTestExpr node is a placeholder that emits the correct
 * value at runtime.  This structure is used so that the testexpr need be
 * evaluated only once.  Note that after parse analysis, the condition
 * expressions always yield boolean.
 *
 * Note: we can test whether a CaseExpr has been through parse analysis
 * yet by checking whether casetype is InvalidOid or not.
 *----------
 */
typedef struct CaseExpr
{
	Expr		xpr;
	/* 表达式结果的类型 */
	Oid			casetype pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			casecollid pg_node_attr(query_jumble_ignore);
	Expr	   *arg;			/* 隐式相等比较参数 */
	List	   *args;			/* 参数(WHEN 子句列表) */
	Expr	   *defresult;		/* 默认结果(ELSE 子句) */
	int			location;		/* 记号位置,未知则为 -1 */
} CaseExpr;

/*
 * CaseWhen - CASE 表达式的一支
 */
typedef struct CaseWhen
{
	Expr		xpr;
	Expr	   *expr;			/* 条件表达式 */
	Expr	   *result;			/* 替换结果 */
	int			location;		/* 记号位置,未知则为 -1 */
} CaseWhen;

/*
 * Placeholder node for the test value to be processed by a CASE expression.
 * This is effectively like a Param, but can be implemented more simply
 * since we need only one replacement value at a time.
 *
 * We also abuse this node type for some other purposes, including:
 *	* Placeholder for the current array element value in ArrayCoerceExpr;
 *	  see build_coercion_expression().
 *	* Nested FieldStore/SubscriptingRef assignment expressions in INSERT/UPDATE;
 *	  see transformAssignmentIndirection().
 *	* Placeholder for intermediate results in some SQL/JSON expression nodes,
 *	  such as JsonConstructorExpr.
 *
 * The uses in CaseExpr and ArrayCoerceExpr are safe only to the extent that
 * there is not any other CaseExpr or ArrayCoerceExpr between the value source
 * node and its child CaseTestExpr(s).  This is true in the parse analysis
 * output, but the planner's function-inlining logic has to be careful not to
 * break it.
 *
 * The nested-assignment-expression case is safe because the only node types
 * that can be above such CaseTestExprs are FieldStore and SubscriptingRef.
 */
typedef struct CaseTestExpr
{
	Expr		xpr;
	Oid			typeId;			/* 替换值的类型 */
	/* 替换值的 typemod */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* 替换值的排序规则 */
	Oid			collation pg_node_attr(query_jumble_ignore);
} CaseTestExpr;

/*
 * ArrayExpr - ARRAY[] 表达式
 *
 * 注:若 multidims 为假,组成表达式都产出 element_typeid
 * 标识标量类型.若为真,组成表达式都产出 element_typeid
 * 的数组(即与 array_typeid 相同);运行时须检查下标兼容。
 * 见上。
 */
typedef struct ArrayExpr
{
	Expr		xpr;
	/* 表达式结果的类型 */
	Oid			array_typeid pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			array_collid pg_node_attr(query_jumble_ignore);
	/* 数组元素的公共类型 */
	Oid			element_typeid pg_node_attr(query_jumble_ignore);
	/* 数组元素或子数组 */
	List	   *elements;
	/* 元素为子数组则为真 */
	bool		multidims pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} ArrayExpr;

/*
 * RowExpr - a ROW() expression
 *
 * Note: the list of fields must have a one-for-one correspondence with
 * physical fields of the associated rowtype, although it is okay for it
 * to be shorter than the rowtype.  That is, the N'th list element must
 * match up with the N'th physical field.  When the N'th physical field
 * is a dropped column (attisdropped) then the N'th list element can just
 * be a NULL constant.  (This case can only occur for named composite types,
 * not RECORD types, since those are built from the RowExpr itself rather
 * than vice versa.)  It is important not to assume that length(args) is
 * the same as the number of columns logically present in the rowtype.
 *
 * colnames provides field names if the ROW() result is of type RECORD.
 * Names *must* be provided if row_typeid is RECORDOID; but if it is a
 * named composite type, colnames will be ignored in favor of using the
 * type's cataloged field names, so colnames should be NIL.  Like the
 * args list, colnames is defined to be one-for-one with physical fields
 * of the rowtype (although dropped columns shouldn't appear in the
 * RECORD case, so this fine point is currently moot).
 */
typedef struct RowExpr
{
	Expr		xpr;
	List	   *args;			/* 字段 */

	/* RECORDOID 或复合类型的 ID */
	Oid			row_typeid pg_node_attr(query_jumble_ignore);

	/*
	 * row_typeid cannot be a domain over composite, only plain composite.  To
	 * create a composite domain value, apply CoerceToDomain to the RowExpr.
	 *
	 * Note: we deliberately do NOT store a typmod.  Although a typmod will be
	 * associated with specific RECORD types at runtime, it will differ for
	 * different backends, and so cannot safely be stored in stored
	 * parsetrees.  We must assume typmod -1 for a RowExpr node.
	 *
	 * We don't need to store a collation either.  The result type is
	 * necessarily composite, and composite types never have a collation.
	 */

	/* 如何显示本节点 */
	CoercionForm row_format pg_node_attr(query_jumble_ignore);

	/* String 列表,或 NIL */
	List	   *colnames pg_node_attr(query_jumble_ignore);

	int			location;		/* 记号位置,未知则为 -1 */
} RowExpr;

/*
 * RowCompareExpr - row-wise comparison, such as (a, b) <= (1, 2)
 *
 * We support row comparison for any operator that can be determined to
 * act like =, <>, <, <=, >, or >= (we determine this by looking for the
 * operator in btree opfamilies).  Note that the same operator name might
 * map to a different operator for each pair of row elements, since the
 * element datatypes can vary.
 *
 * A RowCompareExpr node is only generated for the < <= > >= cases;
 * the = and <> cases are translated to simple AND or OR combinations
 * of the pairwise comparisons.  However, we include = and <> in the
 * RowCompareType enum for the convenience of parser logic.
 */
typedef enum RowCompareType
{
	/* 本枚举值选为匹配 btree 策略号 */
	ROWCOMPARE_LT = 1,			/* BTLessStrategyNumber */
	ROWCOMPARE_LE = 2,			/* BTLessEqualStrategyNumber */
	ROWCOMPARE_EQ = 3,			/* BTEqualStrategyNumber */
	ROWCOMPARE_GE = 4,			/* BTGreaterEqualStrategyNumber */
	ROWCOMPARE_GT = 5,			/* BTGreaterStrategyNumber */
	ROWCOMPARE_NE = 6			/* 无此 btree 策略 */
} RowCompareType;

typedef struct RowCompareExpr
{
	Expr		xpr;

	/* LT LE GE or GT, never EQ or NE */
	RowCompareType rctype;
	/* 成对比较运算符的 OID 列表 */
	List	   *opnos pg_node_attr(query_jumble_ignore);
	/* 含操作符族的 OID 列表 */
	List	   *opfamilies pg_node_attr(query_jumble_ignore);
	/* 比较用排序规则的 OID 列表 */
	List	   *inputcollids pg_node_attr(query_jumble_ignore);
	/* 左侧输入参数 */
	List	   *largs;
	/* 右侧输入参数 */
	List	   *rargs;
} RowCompareExpr;

/*
 * CoalesceExpr - COALESCE 表达式
 */
typedef struct CoalesceExpr
{
	Expr		xpr;
	/* 表达式结果的类型 */
	Oid			coalescetype pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			coalescecollid pg_node_attr(query_jumble_ignore);
	/* 参数 */
	List	   *args;
	/* 记号位置,未知则为 -1 */
	int			location;
} CoalesceExpr;

/*
 * MinMaxExpr - GREATEST 或 LEAST 函数
 */
typedef enum MinMaxOp
{
	IS_GREATEST,
	IS_LEAST
} MinMaxOp;

typedef struct MinMaxExpr
{
	Expr		xpr;
	/* 参数与结果的公共类型 */
	Oid			minmaxtype pg_node_attr(query_jumble_ignore);
	/* 结果排序规则的 OID */
	Oid			minmaxcollid pg_node_attr(query_jumble_ignore);
	/* 函数应使用的排序规则 OID */
	Oid			inputcollid pg_node_attr(query_jumble_ignore);
	/* 要执行的函数 */
	MinMaxOp	op;
	/* 参数 */
	List	   *args;
	/* 记号位置,未知则为 -1 */
	int			location;
} MinMaxExpr;

/*
 * SQLValueFunction - parameterless functions with special grammar productions
 *
 * The SQL standard categorizes some of these as <datetime value function>
 * and others as <general value specification>.  We call 'em SQLValueFunctions
 * for lack of a better term.  We store type and typmod of the result so that
 * some code doesn't need to know each function individually, and because
 * we would need to store typmod anyway for some of the datetime functions.
 * Note that currently, all variants return non-collating datatypes, so we do
 * not need a collation field; also, all these functions are stable.
 */
typedef enum SQLValueFunctionOp
{
	SVFOP_CURRENT_DATE,
	SVFOP_CURRENT_TIME,
	SVFOP_CURRENT_TIME_N,
	SVFOP_CURRENT_TIMESTAMP,
	SVFOP_CURRENT_TIMESTAMP_N,
	SVFOP_LOCALTIME,
	SVFOP_LOCALTIME_N,
	SVFOP_LOCALTIMESTAMP,
	SVFOP_LOCALTIMESTAMP_N,
	SVFOP_CURRENT_ROLE,
	SVFOP_CURRENT_USER,
	SVFOP_USER,
	SVFOP_SESSION_USER,
	SVFOP_CURRENT_CATALOG,
	SVFOP_CURRENT_SCHEMA
} SQLValueFunctionOp;

typedef struct SQLValueFunction
{
	Expr		xpr;
	SQLValueFunctionOp op;		/* 这是哪个函数 */

	/*
	 * Result type/typmod.  Type is fully determined by "op", so no need to
	 * include this Oid in the query jumbling.
	 */
	Oid			type pg_node_attr(query_jumble_ignore);
	int32		typmod;
	int			location;		/* 记号位置,未知则为 -1 */
} SQLValueFunction;

/*
 * XmlExpr - various SQL/XML functions requiring special grammar productions
 *
 * 'name' carries the "NAME foo" argument (already XML-escaped).
 * 'named_args' and 'arg_names' represent an xml_attribute list.
 * 'args' carries all other arguments.
 *
 * Note: result type/typmod/collation are not stored, but can be deduced
 * from the XmlExprOp.  The type/typmod fields are just used for display
 * purposes, and are NOT necessarily the true result type of the node.
 */
typedef enum XmlExprOp
{
	IS_XMLCONCAT,				/* XMLCONCAT(args) */
	IS_XMLELEMENT,				/* XMLELEMENT(name, xml_attributes, args) */
	IS_XMLFOREST,				/* XMLFOREST(xml_attributes) */
	IS_XMLPARSE,				/* XMLPARSE(text, is_doc, preserve_ws) */
	IS_XMLPI,					/* XMLPI(name [, args]) */
	IS_XMLROOT,					/* XMLROOT(xml, version, standalone) */
	IS_XMLSERIALIZE,			/* XMLSERIALIZE(is_document, xmlval, indent) */
	IS_DOCUMENT					/* xmlval IS DOCUMENT */
} XmlExprOp;

typedef enum XmlOptionType
{
	XMLOPTION_DOCUMENT,
	XMLOPTION_CONTENT
} XmlOptionType;

typedef struct XmlExpr
{
	Expr		xpr;
	/* xml 函数 ID */
	XmlExprOp	op;
	/* xml(NAME foo ...) 语法中的名 */
	char	   *name pg_node_attr(query_jumble_ignore);
	/* xml_attributes 的非 XML 表达式 */
	List	   *named_args;
	/* 并行的 String 值列表 */
	List	   *arg_names pg_node_attr(query_jumble_ignore);
	/* 表达式列表 */
	List	   *args;
	/* DOCUMENT 或CONTENT */
	XmlOptionType xmloption pg_node_attr(query_jumble_ignore);
	/* XMLSERIALIZE 的 INDENT 选项 */
	bool		indent;
	/* XMLSERIALIZE 的目标类型/typmod */
	Oid			type pg_node_attr(query_jumble_ignore);
	int32		typmod pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} XmlExpr;

/*
 * JsonEncoding -
 *	JSON ENCODING 子句的表示
 */
typedef enum JsonEncoding
{
	JS_ENC_DEFAULT,				/* 未指定 */
	JS_ENC_UTF8,
	JS_ENC_UTF16,
	JS_ENC_UTF32,
} JsonEncoding;

/*
 * JsonFormatType -
 *	JSON FORMAT 子句所用 JSON 格式的枚举
 */
typedef enum JsonFormatType
{
	JS_FORMAT_DEFAULT,			/* 未指定 */
	JS_FORMAT_JSON,				/* FORMAT JSON [ENCODING ...] */
	JS_FORMAT_JSONB				/* RETURNING 的隐式内部格式
								 * jsonb */
} JsonFormatType;

/*
 * JsonFormat -
 *	JSON FORMAT 子句的表示
 */
typedef struct JsonFormat
{
	NodeTag		type;
	JsonFormatType format_type; /* format type */
	JsonEncoding encoding;		/* JSON 编码 */
	int			location;		/* 记号位置,未知则为 -1 */
} JsonFormat;

/*
 * JsonReturning -
 *	JSON RETURNING 子句的变换表示
 */
typedef struct JsonReturning
{
	NodeTag		type;
	JsonFormat *format;			/* 输出 JSON 格式 */
	Oid			typid;			/* target type Oid */
	int32		typmod;			/* target type modifier */
} JsonReturning;

/*
 * JsonValueExpr -
 *	JSON 值表达式的表示(expr [FORMAT JsonFormat])
 *
 * raw_expr 是用户指定的值,formatted_expr 是通过强制
 * raw_expr 到 FORMAT 子句或包围节点 RETURNING 子句所需
 * 类型而得的值。
 *
 * 反解析 JsonValueExpr 时,get_rule_expr() 打印 raw_expr。
 * 但求值时 formatted_expr 的值优先于 raw_expr。
 * 见上。
 */
typedef struct JsonValueExpr
{
	NodeTag		type;
	Expr	   *raw_expr;		/* 用户指定的表达式 */
	Expr	   *formatted_expr; /* 强制后格式化表达式 */
	JsonFormat *format;			/* FORMAT 子句(若指定) */
} JsonValueExpr;

typedef enum JsonConstructorType
{
	JSCTOR_JSON_OBJECT = 1,
	JSCTOR_JSON_ARRAY = 2,
	JSCTOR_JSON_OBJECTAGG = 3,
	JSCTOR_JSON_ARRAYAGG = 4
} JsonConstructorType;

/*
 * JsonConstructorExpr -
 *	SQL/JSON 构造器上 FuncExpr/Aggref/WindowFunc 的包装
 */
typedef struct JsonConstructorExpr
{
	Expr		xpr;
	JsonConstructorType type;	/* 构造器类型 */
	List	   *args;
	Expr	   *func;			/* 底层 json[b]_xxx() 函数调用 */
	Expr	   *coercion;		/* 强制到 RETURNING 类型 */
	JsonReturning *returning;	/* RETURNING 子句 */
	bool		absent_on_null; /* ABSENT ON NULL? */
	bool		unique;			/* WITH UNIQUE KEYS?(仅 JSON_OBJECT[AGG]) */
	int			location;
} JsonConstructorExpr;

/*
 * JsonValueType -
 *	IS JSON 谓词中 JSON 项类型的表示
 */
typedef enum JsonValueType
{
	JS_TYPE_ANY,				/* IS JSON [VALUE] */
	JS_TYPE_OBJECT,				/* IS JSON OBJECT */
	JS_TYPE_ARRAY,				/* IS JSON ARRAY */
	JS_TYPE_SCALAR				/* IS JSON SCALAR */
} JsonValueType;

/*
 * JsonIsPredicate -
 *	IS JSON 谓词的表示
 */
typedef struct JsonIsPredicate
{
	NodeTag		type;
	Node	   *expr;			/* 主体表达式 */
	JsonFormat *format;			/* FORMAT 子句(若指定) */
	JsonValueType item_type;	/* JSON 项类型 */
	bool		unique_keys;	/* 检查键唯一性? */
	int			location;		/* 记号位置,未知则为 -1 */
} JsonIsPredicate;

/* ----------------
 * NullTest
 *
 * NullTest 表示测试值是否为 NULL 的操作。
 * 执行适当测试并作为布尔 Datum 返回。
 *
 * argisrow 为假时,这只是对空值的测试。
 *
 * argisrow 为真时,输入表达式须产出行类型,节点按 SQL
 * 标准实现 "row IS [NOT] NULL"。这包括在行 Datum 本身
 * 非 NULL 时检查各字段是否为空。
 * 见上。
 *
 * 注:行类型输入且 argisrow==false 的组合并不对应 SQL
 * 记法 "row IS [NOT] NULL";而是表示 "row IS [NOT]
 * DISTINCT FROM NULL"。
 * ----------------
 */

typedef enum NullTestType
{
	IS_NULL, IS_NOT_NULL
} NullTestType;

typedef struct NullTest
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	NullTestType nulltesttype;	/* IS NULL, IS NOT NULL */
	/* T 表示逐字段空检查 */
	bool		argisrow pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} NullTest;

/*
 * BooleanTest
 *
 * BooleanTest 表示判定布尔是 TRUE、FALSE 还是 UNKNOWN
 * (即 NULL)的操作。支持全部六种有意义组合。注:NULL
 * 输入*不*导致 NULL 结果.执行适当测试并作为布尔 Datum
 * 返回。
 */

typedef enum BoolTestType
{
	IS_TRUE, IS_NOT_TRUE, IS_FALSE, IS_NOT_FALSE, IS_UNKNOWN, IS_NOT_UNKNOWN
} BoolTestType;

typedef struct BooleanTest
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	BoolTestType booltesttype;	/* test type */
	int			location;		/* 记号位置,未知则为 -1 */
} BooleanTest;

/*
 * CoerceToDomain
 *
 * CoerceToDomain represents the operation of coercing a value to a domain
 * type.  At runtime (and not before) the precise set of constraints to be
 * checked will be determined.  If the value passes, it is returned as the
 * result; if not, an error is raised.  Note that this is equivalent to
 * RelabelType in the scenario where no constraints are applied.
 */
typedef struct CoerceToDomain
{
	Expr		xpr;
	Expr	   *arg;			/* 输入表达式 */
	Oid			resulttype;		/* 域类型 ID(结果类型) */
	/* 输出 typmod(当前恒为 -1) */
	int32		resulttypmod pg_node_attr(query_jumble_ignore);
	/* 排序规则 OID,无则为 InvalidOid */
	Oid			resultcollid pg_node_attr(query_jumble_ignore);
	/* 如何显示本节点 */
	CoercionForm coercionformat pg_node_attr(query_jumble_ignore);
	int			location;		/* 记号位置,未知则为 -1 */
} CoerceToDomain;

/*
 * Placeholder node for the value to be processed by a domain's check
 * constraint.  This is effectively like a Param, but can be implemented more
 * simply since we need only one replacement value at a time.
 *
 * Note: the typeId/typeMod/collation will be set from the domain's base type,
 * not the domain itself.  This is because we shouldn't consider the value
 * to be a member of the domain if we haven't yet checked its constraints.
 */
typedef struct CoerceToDomainValue
{
	Expr		xpr;
	/* 替换值的类型 */
	Oid			typeId;
	/* 替换值的 typemod */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* 替换值的排序规则 */
	Oid			collation pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} CoerceToDomainValue;

/*
 * Placeholder node for a DEFAULT marker in an INSERT or UPDATE command.
 *
 * This is not an executable expression: it must be replaced by the actual
 * column default expression during rewriting.  But it is convenient to
 * treat it as an expression node during parsing and rewriting.
 */
typedef struct SetToDefault
{
	Expr		xpr;
	/* 替换值的类型 */
	Oid			typeId;
	/* 替换值的 typemod */
	int32		typeMod pg_node_attr(query_jumble_ignore);
	/* 替换值的排序规则 */
	Oid			collation pg_node_attr(query_jumble_ignore);
	/* 记号位置,未知则为 -1 */
	int			location;
} SetToDefault;

/*
 * 表示 [WHERE] CURRENT OF cursor_name 的节点
 *
 * CURRENT OF 有点像 Var,携带被约束目标关系的范围表下标;
 * 这有助于规划期正确放置表达式.但可假定其 levelsup 恒为
 * 零,因语法限制了其可出现位置。另 cvarno 总是真正的 RT
 * 下标,从不为 INNER_VAR 等。
 * 见上。
 *
 * 被引用游标可为为硬编码串,或类型为 REFCURSOR 运行时
 * 参数引用。后者为 plpgsql 方便。
 * 见上。
 */
typedef struct CurrentOfExpr
{
	Expr		xpr;
	Index		cvarno;			/* 目标关系的 RT 下标 */
	char	   *cursor_name;	/* 被引用游标名,或 NULL */
	int			cursor_param;	/* refcursor 参数号,或 0 */
} CurrentOfExpr;

/*
 * NextValueExpr - 从序列取下一值
 *
 * 效果与调用 nextval() 相同,但不检查序列上的权限。用于
 * 身份列,彼时序列是无自身权限的隐式依赖。
 * 见上。
 */
typedef struct NextValueExpr
{
	Expr		xpr;
	Oid			seqid;
	Oid			typeId;
} NextValueExpr;

/*
 * InferenceElem - 唯一索引推断规约的一个元素
 *
 * 大体匹配 IndexElems 的结构,但专用 primnode 使 utility
 * 命令对索引参数的使用与本节点干净分离。
 * 见上。
 */
typedef struct InferenceElem
{
	Expr		xpr;
	Node	   *expr;			/* 要从中推断表达式,或 NULL */
	Oid			infercollid;	/* 排序规则 OID,或 InvalidOid */
	Oid			inferopclass;	/* 属性 opclass  OID,或InvalidOid */
} InferenceElem;

/*--------------------
 * TargetEntry -
 *	   a target entry (used in query target lists)
 *
 * Strictly speaking, a TargetEntry isn't an expression node (since it can't
 * be evaluated by ExecEvalExpr).  But we treat it as one anyway, since in
 * very many places it's convenient to process a whole query targetlist as a
 * single expression tree.
 *
 * In a SELECT's targetlist, resno should always be equal to the item's
 * ordinal position (counting from 1).  However, in an INSERT or UPDATE
 * targetlist, resno represents the attribute number of the destination
 * column for the item; so there may be missing or out-of-order resnos.
 * It is even legal to have duplicated resnos; consider
 *		UPDATE table SET arraycol[1] = ..., arraycol[2] = ..., ...
 * In an INSERT, the rewriter and planner will normalize the tlist by
 * reordering it into physical column order and filling in default values
 * for any columns not assigned values by the original query.  In an UPDATE,
 * after the rewriter merges multiple assignments for the same column, the
 * planner extracts the target-column numbers into a separate "update_colnos"
 * list, and then renumbers the tlist elements serially.  Thus, tlist resnos
 * match ordinal position in all tlists seen by the executor; but it is wrong
 * to assume that before planning has happened.
 *
 * resname is required to represent the correct column name in non-resjunk
 * entries of top-level SELECT targetlists, since it will be used as the
 * column title sent to the frontend.  In most other contexts it is only
 * a debugging aid, and may be wrong or even NULL.  (In particular, it may
 * be wrong in a tlist from a stored rule, if the referenced column has been
 * renamed by ALTER TABLE since the rule was made.  Also, the planner tends
 * to store NULL rather than look up a valid name for tlist entries in
 * non-toplevel plan nodes.)  In resjunk entries, resname should be either
 * a specific system-generated name (such as "ctid") or NULL; anything else
 * risks confusing ExecGetJunkAttribute!
 *
 * ressortgroupref is used in the representation of ORDER BY, GROUP BY, and
 * DISTINCT items.  Targetlist entries with ressortgroupref=0 are not
 * sort/group items.  If ressortgroupref>0, then this item is an ORDER BY,
 * GROUP BY, and/or DISTINCT target value.  No two entries in a targetlist
 * may have the same nonzero ressortgroupref --- but there is no particular
 * meaning to the nonzero values, except as tags.  (For example, one must
 * not assume that lower ressortgroupref means a more significant sort key.)
 * The order of the associated SortGroupClause lists determine the semantics.
 *
 * resorigtbl/resorigcol identify the source of the column, if it is a
 * simple reference to a column of a base table (or view).  If it is not
 * a simple reference, these fields are zeroes.
 *
 * If resjunk is true then the column is a working column (such as a sort key)
 * that should be removed from the final output of the query.  Resjunk columns
 * must have resnos that cannot duplicate any regular column's resno.  Also
 * note that there are places that assume resjunk columns come after non-junk
 * columns.
 *--------------------
 */
typedef struct TargetEntry
{
	Expr		xpr;
	/* 要求值的表达式 */
	Expr	   *expr;
	/* 属性号(见上方注释) */
	AttrNumber	resno;
	/* 列名(可为 NULL) */
	char	   *resname pg_node_attr(query_jumble_ignore);
	/* 被排序/分组子句引用则为非零 */
	Index		ressortgroupref;
	/* 列源表的 OID */
	Oid			resorigtbl pg_node_attr(query_jumble_ignore);
	/* 列在源表中的编号 */
	AttrNumber	resorigcol pg_node_attr(query_jumble_ignore);
	/* 置真以从最终目标列表消除该属性 */
	bool		resjunk pg_node_attr(query_jumble_ignore);
} TargetEntry;


/* ----------------------------------------------------------------
 *					node types for join trees
 *
 * The leaves of a join tree structure are RangeTblRef nodes.  Above
 * these, JoinExpr nodes can appear to denote a specific kind of join
 * or qualified join.  Also, FromExpr nodes can appear to denote an
 * ordinary cross-product join ("FROM foo, bar, baz WHERE ...").
 * FromExpr is like a JoinExpr of jointype JOIN_INNER, except that it
 * may have any number of child nodes, not just two.
 *
 * NOTE: the top level of a Query's jointree is always a FromExpr.
 * Even if the jointree contains no rels, there will be a FromExpr.
 *
 * NOTE: the qualification expressions present in JoinExpr nodes are
 * *in addition to* the query's main WHERE clause, which appears as the
 * qual of the top-level FromExpr.  The reason for associating quals with
 * specific nodes in the jointree is that the position of a qual is critical
 * when outer joins are present.  (If we enforce a qual too soon or too late,
 * that may cause the outer join to produce the wrong set of NULL-extended
 * rows.)  If all joins are inner joins then all the qual positions are
 * semantically interchangeable.
 *
 * NOTE: in the raw output of gram.y, a join tree contains RangeVar,
 * RangeSubselect, and RangeFunction nodes, which are all replaced by
 * RangeTblRef nodes during the parse analysis phase.  Also, the top-level
 * FromExpr is added during parse analysis; the grammar regards FROM and
 * WHERE as separate.
 * ----------------------------------------------------------------
 */

/*
 * RangeTblRef - reference to an entry in the query's rangetable
 *
 * We could use direct pointers to the RT entries and skip having these
 * nodes, but multiple pointers to the same node in a querytree cause
 * lots of headaches, so it seems better to store an index into the RT.
 */
typedef struct RangeTblRef
{
	NodeTag		type;
	int			rtindex;
} RangeTblRef;

/*----------
 * JoinExpr - for SQL JOIN expressions
 *
 * isNatural, usingClause, and quals are interdependent.  The user can write
 * only one of NATURAL, USING(), or ON() (this is enforced by the grammar).
 * If he writes NATURAL then parse analysis generates the equivalent USING()
 * list, and from that fills in "quals" with the right equality comparisons.
 * If he writes USING() then "quals" is filled with equality comparisons.
 * If he writes ON() then only "quals" is set.  Note that NATURAL/USING
 * are not equivalent to ON() since they also affect the output column list.
 *
 * alias is an Alias node representing the AS alias-clause attached to the
 * join expression, or NULL if no clause.  NB: presence or absence of the
 * alias has a critical impact on semantics, because a join with an alias
 * restricts visibility of the tables/columns inside it.
 *
 * join_using_alias is an Alias node representing the join correlation
 * name that SQL:2016 and later allow to be attached to JOIN/USING.
 * Its column alias list includes only the common column names from USING,
 * and it does not restrict visibility of the join's input tables.
 *
 * During parse analysis, an RTE is created for the Join, and its index
 * is filled into rtindex.  This RTE is present mainly so that Vars can
 * be created that refer to the outputs of the join.  The planner sometimes
 * generates JoinExprs internally; these can have rtindex = 0 if there are
 * no join alias variables referencing such joins.
 *----------
 */
typedef struct JoinExpr
{
	NodeTag		type;
	JoinType	jointype;		/* 连接类型 */
	bool		isNatural;		/* 自然连接?将需整形表 */
	Node	   *larg;			/* 左子树 */
	Node	   *rarg;			/* 右子树 */
	/* USING 子句(若有,String 列表) */
	List	   *usingClause pg_node_attr(query_jumble_ignore);
	/* 附着于 USING 子句别名(若有) */
	Alias	   *join_using_alias pg_node_attr(query_jumble_ignore);
	/* 连接上的限定(若有) */
	Node	   *quals;
	/* 用户写的别名子句(若有) */
	Alias	   *alias pg_node_attr(query_jumble_ignore);
	/* 为连接分配 RT 下标,或 0 */
	int			rtindex;
} JoinExpr;

/*----------
 * FromExpr - represents a FROM ... WHERE ... construct
 *
 * This is both more flexible than a JoinExpr (it can have any number of
 * children, including zero) and less so --- we don't need to deal with
 * aliases and so on.  The output column set is implicitly just the union
 * of the outputs of the children.
 *----------
 */
typedef struct FromExpr
{
	NodeTag		type;
	List	   *fromlist;		/* 连接子树列表 */
	Node	   *quals;			/* 连接上的限定(若有) */
} FromExpr;

/*----------
 * OnConflictExpr - 表示 ON CONFLICT DO ... 表达式
 *
 * 优化器需要推断元素列表,以及可选 WHERE 子句以推断唯一
 * 索引。推断出唯一索引(或偶尔多个)供仲裁否走替代
 * ON CONFLICT 路径。
 * 见上。
 *----------
 */
typedef struct OnConflictExpr
{
	NodeTag		type;
	OnConflictAction action;	/* DO NOTHING 或UPDATE? */

	/* Arbiter */
	List	   *arbiterElems;	/* 唯一索引仲裁者表(为
								 * InferenceElem) */
	Node	   *arbiterWhere;	/* unique index arbiter WHERE clause */
	Oid			constraint;		/* 仲裁者 pg_constraint OID */

	/* ON CONFLICT UPDATE */
	List	   *onConflictSet;	/* ON CONFLICT SET TargetEntrys 列表 */
	Node	   *onConflictWhere;	/* 限制 UPDATE 的限定 */
	int			exclRelIndex;	/* excluded 关系的 RT 下标 */
	List	   *exclRelTlist;	/* EXCLUDED 伪关系的 tlist */
} OnConflictExpr;

#endif							/* PRIMNODES_H */
