/*-------------------------------------------------------------------------
 *
 * parse_node.h
 *		解析器内部定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_node.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_NODE_H
#define PARSE_NODE_H

#include "nodes/parsenodes.h"
#include "utils/queryenvironment.h"
#include "utils/relcache.h"


/*  下文若干结构体的前向引用  */
typedef struct ParseState ParseState;
typedef struct ParseNamespaceItem ParseNamespaceItem;
typedef struct ParseNamespaceColumn ParseNamespaceColumn;

/*
 * transformExpr() 区分的表达式种类。许多在语义上
 * 对表达式变换并无区别；我们区分它们
 * 是为了能打印上下文相关的错误消息。
 *
 * 注意：EXPR_KIND_OTHER 核心代码未用，留给可能
 * 需调用 transformExpr() 的扩展。核心代码
 * 不对 EXPR_KIND_OTHER 表达式施加任何
 * 上下文驱动的限制，故调用方须自行检查子查询、聚集、
 * 窗口函数、SRFs 等是否需禁止。
 */
typedef enum ParseExprKind
{
	EXPR_KIND_NONE = 0,			/*  "不在表达式中"  */
	EXPR_KIND_OTHER,			/*  留给扩展  */
	EXPR_KIND_JOIN_ON,			/* JOIN ON */
	EXPR_KIND_JOIN_USING,		/* JOIN USING */
	EXPR_KIND_FROM_SUBSELECT,	/*  FROM 中的子 SELECT  */
	EXPR_KIND_FROM_FUNCTION,	/*  FROM 中的函数  */
	EXPR_KIND_WHERE,			/* WHERE */
	EXPR_KIND_HAVING,			/* HAVING */
	EXPR_KIND_FILTER,			/* FILTER */
	EXPR_KIND_WINDOW_PARTITION, /*  窗口定义 PARTITION BY  */
	EXPR_KIND_WINDOW_ORDER,		/*  窗口定义 ORDER BY  */
	EXPR_KIND_WINDOW_FRAME_RANGE,	/*  带 RANGE 的窗口帧子句  */
	EXPR_KIND_WINDOW_FRAME_ROWS,	/*  带 ROWS 的窗口帧子句  */
	EXPR_KIND_WINDOW_FRAME_GROUPS,	/*  带 GROUPS 的窗口帧子句  */
	EXPR_KIND_SELECT_TARGET,	/*  SELECT 目标列项  */
	EXPR_KIND_INSERT_TARGET,	/*  INSERT 目标列项  */
	EXPR_KIND_UPDATE_SOURCE,	/*  UPDATE 赋值源项  */
	EXPR_KIND_UPDATE_TARGET,	/*  UPDATE 赋值目标项  */
	EXPR_KIND_MERGE_WHEN,		/*  MERGE WHEN [NOT] MATCHED 条件  */
	EXPR_KIND_GROUP_BY,			/* GROUP BY */
	EXPR_KIND_ORDER_BY,			/* ORDER BY */
	EXPR_KIND_DISTINCT_ON,		/* DISTINCT ON */
	EXPR_KIND_LIMIT,			/* LIMIT */
	EXPR_KIND_OFFSET,			/* OFFSET */
	EXPR_KIND_RETURNING,		/* RETURNING */
	EXPR_KIND_VALUES,			/* VALUES */
	EXPR_KIND_VALUES_SINGLE,	/*  单行 VALUES（仅 INSERT）  */
	EXPR_KIND_CHECK_CONSTRAINT, /*  表的 CHECK 约束  */
	EXPR_KIND_DOMAIN_CHECK,		/*  域的 CHECK 约束  */
	EXPR_KIND_COLUMN_DEFAULT,	/*  表列的默认值  */
	EXPR_KIND_FUNCTION_DEFAULT, /*  函数参数的默认值  */
	EXPR_KIND_INDEX_EXPRESSION, /*  索引表达式 */
	EXPR_KIND_INDEX_PREDICATE,	/*  索引谓词  */
	EXPR_KIND_STATS_EXPRESSION, /*  扩展统计表达式  */
	EXPR_KIND_ALTER_COL_TRANSFORM,	/*  ALTER COLUMN TYPE 变换表达式  */
	EXPR_KIND_EXECUTE_PARAMETER,	/*  EXECUTE 中的参数值  */
	EXPR_KIND_TRIGGER_WHEN,		/*  CREATE TRIGGER 的 WHEN 条件  */
	EXPR_KIND_POLICY,			/*  策略中 USING/WITH CHECK  */
	EXPR_KIND_PARTITION_BOUND,	/*  分区边界表达式  */
	EXPR_KIND_PARTITION_EXPRESSION, /*  PARTITION BY 表达式 */
	EXPR_KIND_CALL_ARGUMENT,	/*  CALL 中的过程参数  */
	EXPR_KIND_COPY_WHERE,		/*  COPY FROM 的 WHERE 条件  */
	EXPR_KIND_GENERATED_COLUMN, /*  列的生成表达式  */
	EXPR_KIND_CYCLE_MARK,		/*  环标记值  */
} ParseExprKind;


/*
 * 解析器钩子的函数签名
 */
typedef Node *(*PreParseColumnRefHook) (ParseState *pstate, ColumnRef *cref);
typedef Node *(*PostParseColumnRefHook) (ParseState *pstate, ColumnRef *cref, Node *var);
typedef Node *(*ParseParamRefHook) (ParseState *pstate, ParamRef *pref);
typedef Node *(*CoerceParamHook) (ParseState *pstate, Param *param,
								  Oid targetTypeId, int32 targetTypeMod,
								  int location);


/*
 * 解析分析期间使用的状态信息
 *
 * parentParseState：顶层 ParseState 为 NULL。解析子查询时，
 * 链到外层查询的当前解析状态。
 *
 * p_sourcetext：生成正被分析的原始解析树的
 * 源串，不可用则为 NULL。（该串仅用于
 * 在错误消息中生成光标位置：需将
 * 解析结构中的字节位置转为字符位置的
 * 光标。
 *
 * p_rtable：将成为查询范围表的 RTE 列表。
 * 注意这些项的 relname 与 refname 未必
 * 唯一；按名搜 rtable 是坏主意。
 *
 * p_rteperminfos：RTEPermissionInfo 列表，对应
 * p_rtable 中每个 RTE_RELATION 项。
 *
 * p_joinexprs：与 p_rtable 项关联的 JoinExpr 节点列表。
 * 与 p_rtable 一一对应，但非连接处为 NULLs，
 * 若末尾 RTEs 非连接则可短于 p_rtable。
 *
 * p_nullingrels：与 p_rtable 关联的位图列表，各
 * 含可在解析树当前点置空该关系的外连接 RTE 下标
 * 集合。与 p_rtable 一一对应，
 * 但可短于 p_rtable，缺失项
 * 隐含为空（NULL）。查询无外连接时可
 * 省去工作。
 *
 * p_joinlist：连接项列表（RangeTblRef 与 JoinExpr），
 * 将成为查询顶层 FromExpr 的 fromlist。
 *
 * p_namespace：当前表/列查找用的
 * ParseNamespaceItems 列表。（所列 RTEs 可能只是
 * 整个 rtable 的子集。见下文注释。）
 *
 * p_lateral_active：若正解析本层 LATERAL 子表达式则为真。
 * 此时 p_lateral_only 命名空间项可见，
 * 而 p_lateral_active 为 FALSE 时不可见。
 *
 * p_ctenamespace：当前可见的 CommonTableExprs
 * 与 p_namespace 完全不同，因 CTE
 * 不是 RTE，"可见"指可从其造出 RTE。
 *
 * p_future_ctes：因作用域规则尚不可见的
 * CommonTableExprs（WITH 项）。用于改善错误消息。
 *
 * p_parent_cte：立即包含当前查询的 CommonTableExpr，
 * 若有
 *
 * p_target_relation：目标关系，若查询为 INSERT/UPDATE/DELETE/MERGE
 *
 * p_target_nsitem：目标的 ParseNamespaceItem。
 *
 * p_is_insert：真则按 INSERT 处理赋值表达式，假则
 * 按 UPDATE。注意语句内可改变，如
 * INSERT ON CONFLICT UPDATE。）
 *
 * p_windowdefs：表示 WINDOW 与 OVER 子句的 WindowDefs 列表。
 * 变换表达式时收集，之后再变换
 * （使排序/分组所需的 resjunk_tlist 项
 * 落在查询 tlist 末尾）。WindowDef 在
 * 本列表中从 1 计的位置即引用它的 winref 号。
 *
 * p_expr_kind：当前解析的表达式种类，见上枚举；
 * 非表达式时为 EXPR_KIND_NONE。
 *
 * p_next_resno：下一 TargetEntry.resno，自 1。
 *
 * p_multiassign_exprs：部分处理的 MultiAssignRef 源表达式。
 *
 * p_locking_clause：FOR UPDATE/SHARE 子句，若有。
 *
 * p_locked_from_parent：若父查询层对本子查询整体
 * 施加行锁则为真。
 *
 * p_resolve_unknowns：将未知类型的 SELECT 输出列解析为 TEXT
 * （默认如此）。
 *
 * p_hasAggs、p_hasWindowFuncs 等：若已发现所指
 * 构造则为真。
 *
 * p_last_srf：最近的返回集合 FuncExpr/OpExpr，
 * 若无则为 NULL。
 *
 * p_pre_columnref_hook 等：可选解析器钩子，用于修改
 * ColumnRefs 与 ParamRefs 的解释。
 *
 * p_ref_hook_state：解析器钩子函数的透传状态。
 */
struct ParseState
{
	ParseState *parentParseState;	/*  栈链  */
	const char *p_sourcetext;	/*  源文本，不可用则为 NULL  */
	List	   *p_rtable;		/*  目前范围表  */
	List	   *p_rteperminfos; /*  各 RTE_RELATION 的 RTEPermissionInfo
								 * 列表项（rtable 中）  */
	List	   *p_joinexprs;	/*  p_rtable 中 RTE_JOIN 的 JoinExprs  */
	List	   *p_nullingrels;	/*  显示置空外连接的 Bitmapset  */
	List	   *p_joinlist;		/*  目前连接项（将成为 FromExpr
								 * 的 fromlist）  */
	List	   *p_namespace;	/*  当前可引用的 RTEs（
								 * ParseNamespaceItem) */
	bool		p_lateral_active;	/*  p_lateral_only 项可见？  */
	List	   *p_ctenamespace; /*  CTE 当前命名空间  */
	List	   *p_future_ctes;	/*  尚未进入命名空间的 CTE  */
	CommonTableExpr *p_parent_cte;	/*  本查询的包含 CTE  */
	Relation	p_target_relation;	/*  INSERT/UPDATE/DELETE/MERGE 目标  */
	ParseNamespaceItem *p_target_nsitem;	/*  目标 NSItem，或 NULL  */
	bool		p_is_insert;	/*  按 INSERT 而非 UPDATE 处理赋值  */
	List	   *p_windowdefs;	/*  窗口子句的原始表示  */
	ParseExprKind p_expr_kind;	/*  正解析何种表达式  */
	int			p_next_resno;	/*  下一目标列表 resno  */
	List	   *p_multiassign_exprs;	/*  多赋值的垃圾 tlist 项  */
	List	   *p_locking_clause;	/*  raw FOR UPDATE/SHARE  */
	bool		p_locked_from_parent;	/*  父级已用 FOR UPDATE/SHARE
										 * 标记本子查询  */
	bool		p_resolve_unknowns; /*  未知类型 SELECT 输出解析为
									 * text  */

	QueryEnvironment *p_queryEnv;	/*  当前环境，含外层引用  */

	/*  描述查询中已发现事物的标志：  */
	bool		p_hasAggs;
	bool		p_hasWindowFuncs;
	bool		p_hasTargetSRFs;
	bool		p_hasSubLinks;
	bool		p_hasModifyingCTE;

	Node	   *p_last_srf;		/*  最近集合返回函数/操作符  */

	/*
	 * 可选的解析器回调钩子。除非由
	 * make_parsestate 调用方设置。
	 */
	PreParseColumnRefHook p_pre_columnref_hook;
	PostParseColumnRefHook p_post_columnref_hook;
	ParseParamRefHook p_paramref_hook;
	CoerceParamHook p_coerce_param_hook;
	void	   *p_ref_hook_state;	/*  上述共用的透传链接  */
};

/*
 * 命名空间列表元素。
 *
 * p_names 含本 nsitem 暴露的表名与列名。
 * （通常等于 p_rte->eref，但对 JOIN USING 别名则
 * 等于 p_rte->join_using_alias。因 USING 列是
 * 连接的前 N 列，净效果是仅经本 nsitem
 * 暴露那些连接列。）
 *
 * p_rte 与 p_rtindex 链到下层范围表项，
 * p_perminfo 链到 rteperminfos 项。
 *
 * p_nscolumns 数组含如何构造引用
 * p_names->colnames 所列名称的 Var 的信息。
 *
 * p_rel_visible 置位的项定义哪些 RTEs 可经
 * 限定名访问；p_cols_visible 定义哪些 RTEs 可经
 * 非限定名访问。两者不同，因无别名的 JOIN
 * 不隐藏所含表（故对限定引用必须
 * 可见），但隐藏其列
 * （对列的非限定引用指向 JOIN 而非成员
 * 表，故不得抱怨该引用歧义）。
 * 反之，无别名的子查询不隐藏其所选列，
 * 但隐藏自动生成的关系名（故子查询
 * 列仅对非限定引用可见）。规则的
 * NEW/OLD 等特殊 RTE 也可能仅设一旗。
 * set.
 *
 * 处理 FROM 子句时，命名空间项可能带
 * p_lateral_only，表示仅对 LATERAL
 * 子表达式可见。（pstate 的 p_lateral_active 标志表明
 * 此刻是否在此类子表达式内。）若未置 p_lateral_ok，
 * 实际使用此类命名空间项即为错。或以为
 * 干脆排除可见性更好，但 SQL:2008 措辞
 * 要求如此。亦用 p_lateral_ok
 * 禁止对 UPDATE/DELETE 目标表的 LATERAL 引用。
 *
 * 命名空间列表任何时候都不应含按
 * checkNameSpaceConflicts 规则冲突的两项；但注意那些
 * 规则比"别名必须不同"更复杂，故实践中
 * 搜索命名空间列表的代码须检查歧义引用。
 */
struct ParseNamespaceItem
{
	Alias	   *p_names;		/*  表名与列名  */
	RangeTblEntry *p_rte;		/*  关系的范围表项  */
	int			p_rtindex;		/*  关系在范围表中的下标  */
	RTEPermissionInfo *p_perminfo;	/*  关系的 rteperminfos 项  */
	/*  与 p_names->colnames 等长的数组：  */
	ParseNamespaceColumn *p_nscolumns;	/*  逐列数据  */
	bool		p_rel_visible;	/*  关系名可见？  */
	bool		p_cols_visible; /*  列名可作为非限定引用可见？  */
	bool		p_lateral_only; /*  仅对 LATERAL 可见？  */
	bool		p_lateral_ok;	/*  连接类型允许用？  */
};

/*
 * ParseNamespaceItem 一列的数据。
 *
 * 跟踪构造引用该列的 Var 所需信息
 * （仅用户定义列；系统列引用与
 * 整行引用另行处理）。
 *
 * p_varno 与 p_varattno 标识语义指称，除非引用
 * 指向语义上不等价于任一侧输入列的 JOIN USING 列
 * （因是 FULL 连接或输入列需类型强制）。那些情况下
 * FULL 连接或输入列需类型强制）。那些情况下
 * p_varno 与 p_varattno 指向 JOIN RTE。
 *
 * p_varnosyn 与 p_varattnosyn 或与 p_varno/p_varattno 相同，
 * 或指定列在隐藏语义指称 RTE 的 refname 的
 * 带别名 JOIN RTE 中的位置。（可以是本
 * ParseNamespaceColumn 所在 JOIN RTE，或更低连接层。）
 *
 * 若 RTE 含已删除列，其 ParseNamespaceColumn 结构
 * 全为零。（惯例以 p_varno == 0 检测。）
 */
struct ParseNamespaceColumn
{
	Index		p_varno;		/*  范围表下标 */
	AttrNumber	p_varattno;		/*  列的属性号  */
	Oid			p_vartype;		/* pg_type OID */
	int32		p_vartypmod;	/*  类型修饰符  */
	Oid			p_varcollid;	/*  排序规则 OID/InvalidOid  */
	Index		p_varnosyn;		/*  句法指称的范围表下标  */
	AttrNumber	p_varattnosyn;	/*  句法指称的属性号  */
	bool		p_dontexpand;	/*  不纳入星号展开  */
};

/*  parser_errposition_callback 函数支持  */
typedef struct ParseCallbackState
{
	ParseState *pstate;
	int			location;
	ErrorContextCallback errcallback;
} ParseCallbackState;


extern ParseState *make_parsestate(ParseState *parentParseState);
extern void free_parsestate(ParseState *pstate);
extern int	parser_errposition(ParseState *pstate, int location);

extern void setup_parser_errposition_callback(ParseCallbackState *pcbstate,
											  ParseState *pstate, int location);
extern void cancel_parser_errposition_callback(ParseCallbackState *pcbstate);

extern void transformContainerType(Oid *containerType, int32 *containerTypmod);

extern SubscriptingRef *transformContainerSubscripts(ParseState *pstate,
													 Node *containerBase,
													 Oid containerType,
													 int32 containerTypMod,
													 List *indirection,
													 bool isAssignment);
extern Const *make_const(ParseState *pstate, A_Const *aconst);

#endif							/* PARSE_NODE_H */
