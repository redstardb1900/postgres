/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	查询计划节点的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/plannodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "access/sdir.h"
#include "access/stratnum.h"
#include "common/relpath.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"


/* ----------------------------------------------------------------
 *	节点定义
 * ----------------------------------------------------------------
 */

/* ----------------
 *	PlannedStmt点
 *
 * 规划器输出是以 PlannedStmt 为根的 Plan 树。
 * PlannedStmt 保存执行器所需的"一次性"信息。
 *
 * 为简化 API,也用 PlannedStmt 包装 utility 语句;
 * 此时 commandType == CMD_UTILITY,语句本身在
 * utilityStmt 字段,结构其余部分多为占位。
 * (仍用canSetTag,stmt_location,stmt_len,或queryId.)
 *
 * PlannedStmt 及各类 Plan 都不支持 equal(),
 * 并非不合理,只是目前无此需要。
 * ----------------
 */
typedef struct PlannedStmt
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	CmdType		commandType;	/* select|insert|update|delete|merge|utility */

	uint64		queryId;		/* 查询标识符(自 Query 拷贝) */

	bool		hasReturning;	/* insert|update|delete RETURNING? */

	bool		hasModifyingCTE;	/* WITH含insert|update|delete? */

	bool		canSetTag;		/* 是否设置命令结果标签? */

	bool		transientPlan;	/* TransactionXmin变则重做计划? */

	bool		dependsOnRole;	/* 计划专属当前角色? */

	bool		parallelModeNeeded; /* 执行是否需要并行模式? */

	int			jitFlags;		/* 应执行哪些形式的 JIT */

	struct Plan *planTree;		/* Plan 节点树 */

	List	   *rtable;			/* RangeTblEntry 节点列表 */

	List	   *permInfos;		/* rtable 所需 RTEPermissionInfo 节点
								 * 列表 */

	/* INSERT/UPDATE/DELETE/MERGE 目标关系的 rtable 下标 */
	List	   *resultRelations;	/* RT 下标整数列表,或 NIL */

	List	   *appendRelations;	/* AppendRelInfo 节点列表 */

	List	   *subplans;		/* SubPlan 表达式的 Plan 树;注意
								 * 有些可为 NULL */

	Bitmapset  *rewindPlanIDs;	/* 需要 REWIND 的子计划下标 */

	List	   *rowMarks;		/* PlanRowMark 列表 */

	List	   *relationOids;	/* 计划所依赖关系的 OID */

	List	   *invalItems;		/* 其他依赖,为 PlanInvalItems */

	List	   *paramExecTypes; /* PARAM_EXEC Params 的类型 OID */

	Node	   *utilityStmt;	/* 非空则为本 utility 语句 */

	/* 源串中的语句位置(自 Query 拷贝) */
	int			stmt_location;	/* 起始位置,未知则为 -1 */
	int			stmt_len;		/* 字节长度;0 表示"串的剩余" */
} PlannedStmt;

/* 取与 SubPlan 节点关联的 Plan 的宏 */
#define exec_subplan_get_plan(plannedstmt, subplan) \
	((Plan *) list_nth((plannedstmt)->subplans, (subplan)->plan_id - 1))


/* ----------------
 *	Plan点
 *
 * 所有计划点以Plan结构为第一字段从而"派生",
 * 保证转成 Plan 时一切正常。(执行器中节点指针
 * 常被泛型地转为 Plan*)
 * 见上。
 *
 * 我们从不真正实例化任何 Plan 节点;这只是所有
 * Plan 型节点的公共抽象超类。
 * ----------------
 */
typedef struct Plan
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;

	/*
	 * 计划的估计执行代价(细节见 costsize.c)
	 */
	Cost		startup_cost;	/* 取任何元组前已花费的代价 */
	Cost		total_cost;		/* 总代价(假定取完所有元组) */

	/*
	 * 规划器对本计划步结果规模的估计
	 */
	Cardinality plan_rows;		/* 计划预期发出的行数 */
	int			plan_width;		/* 平均行宽(字节) */

	/*
	 * 并行查询所需信息
	 */
	bool		parallel_aware; /* 启用并行感知逻辑? */
	bool		parallel_safe;	/* 可否作为并行计划一部分? */

	/*
	 * 异步执行所需信息
	 */
	bool		async_capable;	/* 启用异步能力逻辑? */

	/*
	 * 所有 Plan 类型的公共结构数据。
	 */
	int			plan_node_id;	/* 在整个最终计划树中唯一 */
	List	   *targetlist;		/* 本节点要计算的目标列表 */
	List	   *qual;			/* 隐式 AND 的限定条件 */
	struct Plan *lefttree;		/* 输入计划树 */
	struct Plan *righttree;
	List	   *initPlan;		/* InitPlan节点(非相关表达式
								 * 子查询) */

	/*
	 * 参数变化驱动重扫的管理信息
	 *
	 * extParam 含影响本计划节点或其子节点的所有外部
	 * PARAM_EXEC 的 paramID。节点 initPlans 的 setParam
	 * 不含在内,但其 extParams 含在内。
	 *
	 * allParam 含全部 extParam paramID,外加影响本节点的
	 * 局部参数 ID(即其 initplans 的 setParams)。
	 * 这些是影响本节点的_全部_ PARAM_EXEC 参数。
	 */
	Bitmapset  *extParam;
	Bitmapset  *allParam;
} Plan;

/* ----------------
 *	这些定义用以避免"left"/"right"与"inner"/"outer"
 *	的混淆。约定 "left" 计划即 "outer" 计划,"right"
 *	计划即 inner 计划,但这些宏使代码更易读。
 *	见上。
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/* ----------------
 *	Result点 -
 *	无外计划时,求值无变量的目标列表。
 *	有外计划时,返回外计划元组(按目标列表做一层
 *	投影后)。
 *
 * 若 resconstantqual 非 NULL,表示一次性限定测试
 * (不依赖外计划任何变量,故只需求值一次)。
 * 见上。
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
} Result;

/* ----------------
 *	ProjectSet点 -
 *	对外计划输出元组施加含集合返回函数的投影。
 *	见上。
 * ----------------
 */
typedef struct ProjectSet
{
	Plan		plan;
} ProjectSet;

/* ----------------
 *	ModifyTable点 -
 *	把外计划产生的行作用于结果表,
 *	进行插入、更新或删除。
 *
 * 若原命名目标表是分区表或继承树,nominalRelation
 * 与 rootRelation 都含分区根或 appendrel RTE 的 RT
 * 下标,该 RTE 在计划中不再另提。否则 rootRelation
 * 为零。但 nominalRelation 总会设置,因 EXPLAIN 应
 * 声称它是 INSERT/UPDATE/DELETE/MERGE 目标。
 * 见上。
 *
 * 注意 rowMarks 与 epqParam 被假定对所有表有效;
 * 它们不能含跨表变化的信息。
 * ----------------
 */
typedef struct ModifyTable
{
	Plan		plan;
	CmdType		operation;		/* INSERT,UPDATE,DELETE或MERGE */
	bool		canSetTag;		/* 是否设置命令标签/es_processed? */
	Index		nominalRelation;	/* 供 EXPLAIN 使用的父 RT 下标 */
	Index		rootRelation;	/* 分区/继承时的根 RT 下标 */
	bool		partColsUpdated;	/* 层次中否更新了某分区键? */
	List	   *resultRelations;	/* RT 下标整数列表 */
	List	   *updateColnosLists;	/* 每目标表的 update_colnos 列表 */
	List	   *withCheckOptionLists;	/* 每目标表的 WCO 列表 */
	List	   *returningLists; /* 每目标表的 RETURNING tlist */
	List	   *fdwPrivLists;	/* 每目标表的 FDW 私有数据列表 */
	Bitmapset  *fdwDirectModifyPlans;	/* FDW DM 计划下标 */
	List	   *rowMarks;		/* PlanRowMarks(仅非锁定) */
	int			epqParam;		/* EvalPlanQual 重求值用 Param  ID */
	OnConflictAction onConflictAction;	/* ON CONFLICT 动作 */
	List	   *arbiterIndexes; /* ON CONFLICT 仲裁索引 OID 列表  */
	List	   *onConflictSet;	/* INSERT ON CONFLICT DO UPDATE目标表 */
	List	   *onConflictCols; /* onConflictSet 的目标列号 */
	Node	   *onConflictWhere;	/* ON CONFLICT UPDATE 的 WHERE */
	Index		exclRelRTI;		/* EXCLUDED 伪关系的 RTI */
	List	   *exclRelTlist;	/* EXCLUDED 伪关系的 tlist */
	List	   *mergeActionLists;	/* 每目标表的 MERGE 动作
									 * 表 */
} ModifyTable;

struct PartitionPruneInfo;		/* 对下方结构的前向引用 */

/* ----------------
 *	Append点 -
 *	生成各子计划结果的串联。
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	Bitmapset  *apprelids;		/* 本节点形成的 appendrel 的 RTI */
	List	   *appendplans;
	int			nasyncplans;	/* 异步计划数 */

	/*
	 * 本下标之前的全部 appendplans 为非部分计划。自此
	 * 下标起的 appendplans 为部分计划。
	 */
	int			first_partial_plan;

	/* 运行时子计划剪枝信息;不做则为 NULL */
	struct PartitionPruneInfo *part_prune_info;
} Append;

/* ----------------
 *	MergeAppend点 -
 *	合并预排序子计划的结果以保持次序。
 * ----------------
 */
typedef struct MergeAppend
{
	Plan		plan;

	/* 本节点形成的 appendrel 的 RTI */
	Bitmapset  *apprelids;

	List	   *mergeplans;

	/* 这些字段恰如 Sort 结构中的排序键信息: */

	/* 排序键列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* 用以排序的运算符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/* 运行时子计划剪枝信息;不做则为 NULL */
	struct PartitionPruneInfo *part_prune_info;
} MergeAppend;

/* ----------------
 *	RecursiveUnion点 -
 *	生成两个子计划的递归并。
 *
 * "outer" 子计划恒为非递归项,"inner" 子计划为
 * 递归项。
 * ----------------
 */
typedef struct RecursiveUnion
{
	Plan		plan;

	/* 表示工作表的 Param 的 ID */
	int			wtParam;

	/* UNION ALL 时其余字段为零/空 */

	/* 查重列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *dupColIdx pg_node_attr(array_size(numCols));

	/* 用于比较的相等运算符 */
	Oid		   *dupOperators pg_node_attr(array_size(numCols));
	Oid		   *dupCollations pg_node_attr(array_size(numCols));

	/* 输入中估计的组数 */
	long		numGroups;
} RecursiveUnion;

/* ----------------
 *	BitmapAnd点 -
 *	生成各子计划结果的交。
 *
 * 子计划须产出元组位图。计划的 targetlist 与 qual
 * 字段未用且恒为 NIL。
 * ----------------
 */
typedef struct BitmapAnd
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapAnd;

/* ----------------
 *	BitmapOr点 -
 *	生成各子计划结果的并。
 *
 * 子计划须产出元组位图。计划的 targetlist 与 qual
 * 字段未用且恒为 NIL。
 * ----------------
 */
typedef struct BitmapOr
{
	Plan		plan;
	bool		isshared;
	List	   *bitmapplans;
} BitmapOr;

/*
 * ==========
 * 扫描点
 *
 * Scan 是所有关系扫描计划类型继承的抽象类型。
 * ==========
 */
typedef struct Scan
{
	pg_node_attr(abstract)

	Plan		plan;
	Index		scanrelid;		/* relid 是范围表下标 */
} Scan;

/* ----------------
 *	顺序扫描节点
 * ----------------
 */
typedef struct SeqScan
{
	Scan		scan;
} SeqScan;

/* ----------------
 *	表抽样扫描节点
 * ----------------
 */
typedef struct SampleScan
{
	Scan		scan;
	/* 用结构指针以免在此包含 parsenodes.h */
	struct TableSampleClause *tablesample;
} SampleScan;

/* ----------------
 *	索引扫描点
 *
 * indexqualorig 是隐式 AND 的索引限定表达式列表,各
 * 保持查询 WHERE 中的原形。每项形如 (indexkey OP
 * comparisonval) 或 (comparisonval OP indexkey)。
 * indexkey 是引用索引基表列的 Var 或表达式。
 * comparisonval 可为任意表达式,但不使用基表任何列。
 * 表达式按索引列位置排序(引用同一索引列项可任意
 * 次序)。仅当须复核有损 indexqual 时运行期才用
 * indexqualorig。
 * 见上。
 *
 * indexqual 形式相同,但必要时已交换使索引键在左,
 * 且索引键被标识索引列的 Var 替换(varno 为 INDEX_VAR,
 * varattno 为索引列号)。
 * 见上。
 *
 * indexorderbyorig 类似是由索引实现的 ORDER BY 表达式
 * 原形,而 indexorderby 已改成左侧为索引列 Vars。此处
 * 多表达式须严格按 ORDER BY 次序,未必是索引列次序。
 * 只提供表达式,不含 ORDER BY SortGroupClauses 的辅助
 * 排序信息;假定顶层运算符可完全确定排序。索引无法
 * 算出精确次序时,运行期用 indexorderbyorig 复核次序;
 * EXPLAIN 也需要它。
 * 见上。
 * 见上。
 *
 * indexorderbyops 是排序 ORDER BY 表达式所用运算符的
 * OID 列表。与 indexorderbyorig 一起用于运行时复核
 * 次序。(注:indexorderby/indexorderbyorig/indexorderbyops
 * 用于 amcanorderbyop 情形,非 amcanorder。)
 *
 * indexorderdir 指定扫描方向,用于 amcanorder 索引
 * (其他索引应为"无所谓")。
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	Oid			indexid;		/* 待扫索引的 OID */
	List	   *indexqual;		/* 索引限定列表(通常为 OpExprs) */
	List	   *indexqualorig;	/* 原形相同内容 */
	List	   *indexorderby;	/* 索引ORDER BY表达式表 */
	List	   *indexorderbyorig;	/* 原形相同内容 */
	List	   *indexorderbyops;	/* ORDER BY表达式排序运算符OID */
	ScanDirection indexorderdir;	/* 向前、向后或无所谓 */
} IndexScan;

/* ----------------
 *	仅索引扫描节点
 *
 * IndexOnlyScan 与 IndexScan 很相似,但指定仅索引扫描,
 * 数据来自索引而非堆.因此计划节点目标列表、qual 与
 * 索引表达式中*全部* Vars 引用索引列且 varno=INDEX_VAR.
 * 见上。
 *
 * 复核有损索引运算符时几乎可直接对索引输出元组用
 * indexqual,但对不可取回的索引列限定不行。故需
 * recheckqual:与 indexqual 同条件,但只用可取回的索引
 * 列.(若不可能则不生成仅索引扫描.例如索引在可取回
 * 列 ind1 有表列 x,在不可取回列 ind2 有表达式 f(x),则
 * 对 f(x) 可索引查询在 indexqual 用 ind2、在 recheckqual
 * 用 f(ind1)。若无 ind1 列则禁止仅索引扫描。)
 * 见上。
 * 见上。
 * 见上。
 *
 * 目前不需要可复核的 indexorderby 等价物,因不支持
 * 索引 ORDER BY 中的有损运算符。
 *
 * 为帮 EXPLAIN 解释显示用的索引 Vars,提供 indextlist,
 * 以目标列表表示索引内容,每索引列一个 TLE.此列表中
 *  Vars 引用基表,且是计划节点中唯一可含此类 Vars 的
 * 字段。另为 setrefs.c 方便,若对应索引 AM 无法重构列,
 * indextlist 中的 TLE 标为 resjunk。
 * 见上。
 * 见上。
 * ----------------
 */
typedef struct IndexOnlyScan
{
	Scan		scan;
	Oid			indexid;		/* 待扫索引的 OID */
	List	   *indexqual;		/* 索引限定列表(通常为 OpExprs) */
	List	   *recheckqual;	/* 可复核形式的索引限定 */
	List	   *indexorderby;	/* 索引ORDER BY表达式表 */
	List	   *indextlist;		/* 描述索引列的 TargetEntry 列表 */
	ScanDirection indexorderdir;	/* 向前、向后或无所谓 */
} IndexOnlyScan;

/* ----------------
 *	位图索引扫描点
 *
 * BitmapIndexScan 交付潜在元组位置位图;自身不访问
 * 堆.位图由祖先 BitmapHeapScan 使用,可能先经中间
 * BitmapAnd 和/或 BitmapOr 与其他 BitmapIndexScan 结果
 * 合并。
 * 见上。
 *
 * 各字段含义与 IndexScan 相同,只是不存方向标志,因
 * 方向无关紧要。
 *
 * BitmapIndexScan 计划节点中 targetlist 与 qual 未用且
 * 恒为 NIL。indexqualorig 运行期也不用,但为 EXPLAIN
 * 保留。
 * ----------------
 */
typedef struct BitmapIndexScan
{
	Scan		scan;
	Oid			indexid;		/* 待扫索引的 OID */
	bool		isshared;		/* 置位则建共享位图 */
	List	   *indexqual;		/* 索引限定列表(OpExprs) */
	List	   *indexqualorig;	/* 原形相同内容 */
} BitmapIndexScan;

/* ----------------
 *	位图顺序扫描节点
 *
 * 需要输入索引扫描所用限定条件的副本,因多种情形须
 * 复核限定;例如位图对页上满足索引条件具体行有损时。
 * 见上。
 * 见上。
 * ----------------
 */
typedef struct BitmapHeapScan
{
	Scan		scan;
	List	   *bitmapqualorig; /* 标准表达式形式索引限定 */
} BitmapHeapScan;

/* ----------------
 *	tid扫描点
 *
 * tidquals 是隐式 OR 的限定表达式列表,形如
 * "CTID = 伪常量"、"CTID = ANY(伪常量数组)",
 * 或该关系的 CurrentOfExpr。
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	List	   *tidquals;		/* 涉及 CTID = 某值的限定 */
} TidScan;

/* ----------------
 *	tid 范围扫描点
 *
 * tidrangequals 是隐式 AND 的限定表达式列表,形如
 * "CTID relop 伪常量",relop 为 >、>=、<、<= 之一。
 * ----------------
 */
typedef struct TidRangeScan
{
	Scan		scan;
	List	   *tidrangequals;	/* 涉及 CTID op 某值的限定 */
} TidRangeScan;

/* ----------------
 *	子查询扫描点
 *
 * SubqueryScan 用于扫描范围表中子查询的输出。常需在
 * 子查询计划之上再加一层计划节点做表达式求值(不能
 * 贸然推进子查询以免改语义)。虽非扫描物理关系,仍
 * 令其派生自 Scan 以便共享代码。
 * 见上。
 * 见上。
 *
 * SubqueryScanStatus 缓存节点的 trivial_subqueryscan
 * 属性。SUBQUERY_SCAN_UNKNOWN 表示尚未判定。仅规划期
 * 使用。
 *
 * 注意:子计划存在类型专用 subplan 字段,而非通用
 * lefttree.因不希望计划树遍历例程在不知改 Query 上下文
 * 的情况下递归进子计划。
 * 见上。
 * ----------------
 */
typedef enum SubqueryScanStatus
{
	SUBQUERY_SCAN_UNKNOWN,
	SUBQUERY_SCAN_TRIVIAL,
	SUBQUERY_SCAN_NONTRIVIAL
} SubqueryScanStatus;

typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
	SubqueryScanStatus scanstatus;
} SubqueryScan;

/* ----------------
 *	FunctionScan 点
 * ----------------
 */
typedef struct FunctionScan
{
	Scan		scan;
	List	   *functions;		/* RangeTblFunction 节点列表 */
	bool		funcordinality; /* WITH ORDINALITY */
} FunctionScan;

/* ----------------
 *	ValuesScan 点
 * ----------------
 */
typedef struct ValuesScan
{
	Scan		scan;
	List	   *values_lists;	/* 表达式列表的列表 */
} ValuesScan;

/* ----------------
 *	TableFunc 扫描点
 * ----------------
 */
typedef struct TableFuncScan
{
	Scan		scan;
	TableFunc  *tablefunc;		/* 表函数节点 */
} TableFuncScan;

/* ----------------
 *	CteScan 点
 * ----------------
 */
typedef struct CteScan
{
	Scan		scan;
	int			ctePlanId;		/* CTE  init SubPlan 的 ID */
	int			cteParam;		/* 表示 CTE 输出的 Param 的 ID */
} CteScan;

/* ----------------
 *	NamedTuplestoreScan 点
 * ----------------
 */
typedef struct NamedTuplestoreScan
{
	Scan		scan;
	char	   *enrname;		/* 临时命名关系的给定名 */
} NamedTuplestoreScan;

/* ----------------
 *	WorkTableScan 点
 * ----------------
 */
typedef struct WorkTableScan
{
	Scan		scan;
	int			wtParam;		/* 表示工作表的 Param 的 ID */
} WorkTableScan;

/* ----------------
 *	ForeignScan 点
 *
 * fdw_exprs 与 fdw_private 都由外部数据包装器控制,但
 * fdw_exprs 假定含表达式树并由规划器相应后处理;
 * fdw_private 不会。两列表中一切须能被 copyObject()
 * 拷贝。存任意字节块一种办法是表示为 bytea Const。
 * 但通常更好选能被 nodeToString() 有用转储的表示。
 * 见上。
 * 见上。
 *
 * fdw_scan_tlist 是描述扫描元组内容的目标列表;若扫描
 * 元组匹配外表声明行类型(简单外表扫描常态)可为 NIL。
 * (若计划节点表示外部连接,则必须有 fdw_scan_tlist,因
 * 系统目录无可用行类型。)提供 fdw_scan_tlist 时,节点
 * tlist 与 quals 中的 Vars 须 varno=INDEX_VAR,且 varattnos
 * 对应 fdw_scan_tlist 的 resnos(亦即实际扫描元组的列号)。
 * fdw_scan_tlist 从不真正执行;只保存描述扫描元组各列
 * 内容的表达式树。
 * 见上。
 * 见上。
 *
 * fdw_recheck_quals 应含核心系统传给 FDW 但未加入
 * scan.plan.qual 的限定,即远程检查的限定。EvalPlanQual
 * 复核时行为正确需要它。
 * 见上。
 *
 * 计划节点表示外部连接时 scan.scanrelid 为零,须查阅
 * fs_relids 识别连接关系。(简单扫描时 fs_relids 也有效,
 * 但总与 scan.scanrelid 一致。)fs_relids 含外连接;
 * fs_base_relids 不含。
 *
 * 若 FDW 的 PlanDirectModify() 回调决定改用 ForeignScan
 * 在远程服务器直接做 UPDATE/DELETE,则设置 operation 与
 * resultRelation 标识操作类型与目标关系。仅当修改*完全*
 * 在远程执行时才设置这些字段;否则由本地 ModifyTable
 * 驱动修改,operation 留为 CMD_SELECT。
 * 见上。
 * ----------------
 */
typedef struct ForeignScan
{
	Scan		scan;
	CmdType		operation;		/* SELECT/INSERT/UPDATE/DELETE */
	Index		resultRelation; /* 直接修改目标的 RT 下标 */
	Oid			checkAsUser;	/* 以何用户执行扫描;0 表示
								 * 按当前用户检查 */
	Oid			fs_server;		/* 外部服务器 OID */
	List	   *fdw_exprs;		/* FDW 可能求值的表达式 */
	List	   *fdw_private;	/* FDW 的私有数据 */
	List	   *fdw_scan_tlist; /* 描述扫描元组的可选 tlist */
	List	   *fdw_recheck_quals;	/* 不在 scan.plan.qual 中原限定 */
	Bitmapset  *fs_relids;		/* 本扫描生成的基表+OJ RTI */
	Bitmapset  *fs_base_relids; /* 本扫描生成的基表 RTI */
	bool		fsSystemCol;	/* 是否需要任何"系统列" */
} ForeignScan;

/* ----------------
 *	CustomScan 节点
 *
 * ForeignScan 的 fdw_exprs、fdw_private、fdw_scan_tlist
 * 与 fs_relids 字段的注释同样适用于 CustomScan 的
 * custom_exprs、custom_private、custom_scan_tlist 与
 * custom_relids。连接时 scan.scanrelid 置零的约定亦同。
 *
 * 注意因 Plan 树可拷贝,定制扫描提供者*须*把所需计划
 * 数据都放进这些字段;把 CustomScan 嵌进更大结构不行。
 * 见上。
 * ----------------
 */
struct CustomScanMethods;

typedef struct CustomScan
{
	Scan		scan;
	uint32		flags;			/* CUSTOMPATH_* 标志掩码,见
								 * nodes/extensible.h */
	List	   *custom_plans;	/* Plan 节点列表(若有) */
	List	   *custom_exprs;	/* 定制代码可能求值的表达式 */
	List	   *custom_private; /* 定制代码的私有数据 */
	List	   *custom_scan_tlist;	/* 描述扫描元组的可选 tlist */
	Bitmapset  *custom_relids;	/* 本扫描生成的 RTI */

	/*
	 * 注意:CustomScan 的 method 字段须指向回调函数的静态
	 * 表。故不拷贝该表本身,只引用原表。
	 * 见上。
	 */
	const struct CustomScanMethods *methods;
} CustomScan;

/*
 * ==========
 * 连接点
 * ==========
 */

/* ----------------
 *	Join 点
 *
 * jointype: 左右子树元组的连接规则
 * inner_unique: 每个外元组至多匹配一个内元组
 * joinqual: 来自 JOIN/ON 或 JOIN/USING 的限定
 *	(plan.qual 含来自 WHERE 的条件)
 *
 * jointype 为 INNER 时,joinqual 与 plan.qual 语义可互换.
 * OUTER 连接类型则*不可*互换;仅 joinqual 用于判定是否
 * 找到匹配以决定否生成空扩展元组.(但返回元组前仍
 * 应用 plan.qual。)外连接时,仅 joinquals 可用作归并或
 * 哈希连接的归并/哈希条件。
 * 见上。
 * 见上。
 *
 * 若 joinquals 使得任一给定外元组至多匹配一个内元组,
 * 则置 inner_unique。执行器可跳过继续搜索匹配。(须仅
 * 由 joinquals 可证,忽略 plan.qual,因执行器在该处测试。)
 * 见上。
 * ----------------
 */
typedef struct Join
{
	pg_node_attr(abstract)

	Plan		plan;
	JoinType	jointype;
	bool		inner_unique;
	List	   *joinqual;		/* JOIN 限定(附加于 plan.qual) */
} Join;

/* ----------------
 *		nest loop join node
 *
 * The nestParams list identifies any executor Params that must be passed
 * into execution of the inner subplan carrying values from the current row
 * of the outer subplan.  Currently we restrict these values to be simple
 * Vars, but perhaps someday that'd be worth relaxing.  (Note: during plan
 * creation, the paramval can actually be a PlaceHolderVar expression; but it
 * must be a Var with varno OUTER_VAR by the time it gets to the executor.)
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;
	List	   *nestParams;		/* NestLoopParam 节点列表 */
} NestLoop;

typedef struct NestLoopParam
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	int			paramno;		/* 要设置的 PARAM_EXEC Param 编号 */
	Var		   *paramval;		/* 赋给 Param 的外关系 Var */
} NestLoopParam;

/* ----------------
 *		merge join node
 *
 * The expected ordering of each mergeable column is described by a btree
 * opfamily OID, a collation OID, a direction (BTLessStrategyNumber or
 * BTGreaterStrategyNumber) and a nulls-first flag.  Note that the two sides
 * of each mergeclause may be of different datatypes, but they are ordered the
 * same way according to the common opfamily and collation.  The operator in
 * each mergeclause must be an equality operator of the indicated opfamily.
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;

	/* Can we skip mark/restore calls? */
	bool		skip_mark_restore;

	/* mergeclauses as expression trees */
	List	   *mergeclauses;

	/* these are arrays, but have the same length as the mergeclauses list: */

	/* per-clause OIDs of btree opfamilies */
	Oid		   *mergeFamilies pg_node_attr(array_size(mergeclauses));

	/* per-clause OIDs of collations */
	Oid		   *mergeCollations pg_node_attr(array_size(mergeclauses));

	/* per-clause ordering (ASC or DESC) */
	int		   *mergeStrategies pg_node_attr(array_size(mergeclauses));

	/* per-clause nulls ordering */
	bool	   *mergeNullsFirst pg_node_attr(array_size(mergeclauses));
} MergeJoin;

/* ----------------
 *	哈希连接点
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	List	   *hashclauses;
	List	   *hashoperators;
	List	   *hashcollations;

	/*
	 * 对外计划元组做哈希表达式表,以便在内计划的哈希
	 * 表中查找。
	 */
	List	   *hashkeys;
} HashJoin;

/* ----------------
 *	物化节点
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
} Material;

/* ----------------
 *	memoize 点
 * ----------------
 */
typedef struct Memoize
{
	Plan		plan;

	/* 下列两个数组的大小 */
	int			numKeys;

	/* 各键的哈希运算符 */
	Oid		   *hashOperators pg_node_attr(array_size(numKeys));

	/* 各键的排序规则 */
	Oid		   *collations pg_node_attr(array_size(numKeys));

	/* 含参数的表达式形式的缓存键 */
	List	   *param_exprs;

	/*
	 * 存入第一个元组后是否应将缓存项标为完整。
	 * 见上。
	 */
	bool		singlerow;

	/*
	 * 缓存键应按位比较则为真,用哈希相等运算则为假
	 * 见上。
	 */
	bool		binary_mode;

	/*
	 * 规划器预期缓存可容纳的最大项数,未知则为 0
	 * 见上。
	 */
	uint32		est_entries;

	/* paramids from param_exprs */
	Bitmapset  *keyparamids;
} Memoize;

/* ----------------
 *	排序点
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;

	/* 排序键列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* 用以排序的运算符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));
} Sort;

/* ----------------
 *	增量排序节点
 * ----------------
 */
typedef struct IncrementalSort
{
	Sort		sort;
	int			nPresortedCols; /* 已预排序的列数 */
} IncrementalSort;

/* ---------------
 *	group 点 -
 *	用于指定了 GROUP BY(但无聚合)的查询。
 *	输入须已按分组列预排序。
 * ---------------
 */
typedef struct Group
{
	Plan		plan;

	/* 分组列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* 用于比较的相等运算符 */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));
} Group;

/* ---------------
 *	聚合节点
 *
 * Agg 点实现普通或分组聚合.分组聚合可处理预排序
 * 或未排序输入;后者策略用内部哈希表。
 * 见上。
 *
 * 注意没有关于待算聚合函数直接信息。执行器启动时
 * 扫描节点 tlist 与 quals 找到它们。(可能没有聚合函数:
 * 常量折叠优化掉,或用 Agg 节点实现基于哈希的分组。)
 * 见上。
 * 见上。
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;

	/* 基本策略,见 nodes.h */
	AggStrategy aggstrategy;

	/* 聚合拆分模式,见 nodes.h */
	AggSplit	aggsplit;

	/* 分组列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* 用于比较的相等运算符 */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));

	/* 输入中估计的组数 */
	long		numGroups;

	/* 供按引用传递转移数据 */
	uint64		transitionSpace;

	/* Aggref 输入所用 Params 的 ID */
	Bitmapset  *aggParams;

	/* 注:规划器仅在 HASHED/MIXED 时提供 numGroups 与 aggParams */

	/* 要使用分组集 */
	List	   *groupingSets;

	/* 链式 Agg/Sort 节点 */
	List	   *chain;
} Agg;

/* ----------------
 *	窗口聚合节点
 * ----------------
 */
typedef struct WindowAgg
{
	Plan		plan;

	/* 窗口函数引用的 ID */
	Index		winref;

	/* 分区子句中的列数 */
	int			partNumCols;

	/* 它们在目标列表中下标 */
	AttrNumber *partColIdx pg_node_attr(array_size(partNumCols));

	/* 分区列的相等运算符 */
	Oid		   *partOperators pg_node_attr(array_size(partNumCols));

	/* 分区列的排序规则 */
	Oid		   *partCollations pg_node_attr(array_size(partNumCols));

	/* 排序子句中的列数 */
	int			ordNumCols;

	/* 它们在目标列表中下标 */
	AttrNumber *ordColIdx pg_node_attr(array_size(ordNumCols));

	/* 排序列的相等运算符 */
	Oid		   *ordOperators pg_node_attr(array_size(ordNumCols));

	/* 排序列的排序规则 */
	Oid		   *ordCollations pg_node_attr(array_size(ordNumCols));

	/* frame_clause 选项,见 WindowDef */
	int			frameOptions;

	/* 起始界表达式(若有) */
	Node	   *startOffset;

	/* 结束界表达式(若有) */
	Node	   *endOffset;

	/* 帮助短路执行的限定 */
	List	   *runCondition;

	/* 供 EXPLAIN 显示的 runCondition */
	List	   *runConditionOrig;

	/* 下列字段用于 RANGE offset PRECEDING/FOLLOWING: */

	/* startOffset 的 in_range 函数 */
	Oid			startInRangeFunc;

	/* endOffset 的 in_range 函数 */
	Oid			endInRangeFunc;

	/* in_range 测试排序规则 */
	Oid			inRangeColl;

	/* in_range 测试用 ASC 排序? */
	bool		inRangeAsc;

	/* in_range 测试空值排前? */
	bool		inRangeNullsFirst;

	/*
	 * 除最靠近计划根的 WindowAgg 外皆为假
	 * 见上.
	 */
	bool		topWindow;
} WindowAgg;

/* ----------------
 *	unique 点
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;

	/* 查重列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *uniqColIdx pg_node_attr(array_size(numCols));

	/* 用于比较的相等运算符 */
	Oid		   *uniqOperators pg_node_attr(array_size(numCols));

	/* 相等比较的排序规则 */
	Oid		   *uniqCollations pg_node_attr(array_size(numCols));
} Unique;

/* ------------
 *	gather 点
 *
 * 注:rescan_param 是 PARAM_EXEC 参数槽的 ID。该槽从
 * 不含值,但 Gather 节点重扫时必须标它已变。子并行感知
 * 扫描点标为依赖该参数,使重扫机制知道其输出可能跨
 * 重扫变化.有时不需要重扫 Param,则 rescan_param 为 -1。
 * 见上。
 * 见上。
 * ------------
 */
typedef struct Gather
{
	Plan		plan;
	int			num_workers;	/* 计划的工作进程数 */
	int			rescan_param;	/* 发信号重扫的 Param 的 ID,或 -1 */
	bool		single_copy;	/* 计划勿执行超过一次 */
	bool		invisible;		/* 抑制 EXPLAIN 显示(供测试)? */
	Bitmapset  *initParam;		/* param id's of initplans which are referred
								 * at gather or one of it's child node */
} Gather;

/* ------------
 *		gather merge node
 * ------------
 */
typedef struct GatherMerge
{
	Plan		plan;

	/* 计划的工作进程数 */
	int			num_workers;

	/* 发信号重扫的 Param 的 ID,或 -1 */
	int			rescan_param;

	/* 其余字段恰如 Sort 结构中的排序键信息 */

	/* 排序键列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* 用以排序的运算符 OID */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* 排序规则 OID */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST 方向 */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/*
	 * param id's of initplans which are referred at gather merge or one of
	 * it's child node
	 */
	Bitmapset  *initParam;
} GatherMerge;

/* ----------------
 *	哈希构建点
 *
 * 若执行器应尝试倾斜连接优化,则 skewTable/skewColumn/
 * skewInherit 标识外关系连接键列,可从此取相关 MCV
 * 统计。
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;

	/*
	 * 对 Hash 外计划元组做哈希表达式列表,以便放入哈希
	 * 表。
	 */
	List	   *hashkeys;		/* 哈希连接条件的哈希键 */
	Oid			skewTable;		/* 外连接键的表 OID,或 InvalidOid */
	AttrNumber	skewColumn;		/* 外连接键的列号,或零 */
	bool		skewInherit;	/* 外连接关系是否为继承树? */
	/* 其余信息在父 HashJoin 节点中 */
	Cardinality rows_total;		/* 若 parallel_aware 则估计总行数 */
} Hash;

/* ----------------
 *		setop node
 * ----------------
 */
typedef struct SetOp
{
	Plan		plan;

	/* 做什么,见 nodes.h */
	SetOpCmd	cmd;

	/* 怎么做,见 nodes.h */
	SetOpStrategy strategy;

	/* 查重列数 */
	int			numCols;

	/* 它们在目标列表中下标 */
	AttrNumber *dupColIdx pg_node_attr(array_size(numCols));

	/* 用于比较的相等运算符 */
	Oid		   *dupOperators pg_node_attr(array_size(numCols));
	Oid		   *dupCollations pg_node_attr(array_size(numCols));

	/* 标志列位置(若有) */
	AttrNumber	flagColIdx;

	/* 第一输入关系的标志值 */
	int			firstFlag;

	/* 输入中估计的组数 */
	long		numGroups;
} SetOp;

/* ----------------
 *	锁行节点
 *
 * rowMarks 标识本节点要锁的关系;应是顶层 PlannedStmt
 * 所列 rowMarks 子集。epqParam 其下所有扫描节点须
 * 依赖的 Param,用于 EvalPlanQual 期间强制重求值计划。
 * 见上。
 * ----------------
 */
typedef struct LockRows
{
	Plan		plan;
	List	   *rowMarks;		/* PlanRowMark 列表 */
	int			epqParam;		/* EvalPlanQual 重求值用 Param  ID */
} LockRows;

/* ----------------
 *	limit 点
 *
 * 注:自 Postgres 8.2 起,offset 与 count 表达式预期产出
 * int8,而非先前的 int4。
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;

	/* OFFSET 参数,无则为 NULL */
	Node	   *limitOffset;

	/* COUNT 参数,无则为 NULL */
	Node	   *limitCount;

	/* limit type */
	LimitOption limitOption;

	/* 查相似列数  */
	int			uniqNumCols;

	/* 它们在目标列表中下标 */
	AttrNumber *uniqColIdx pg_node_attr(array_size(uniqNumCols));

	/* 用于比较的相等运算符 */
	Oid		   *uniqOperators pg_node_attr(array_size(uniqNumCols));

	/* 相等比较的排序规则 */
	Oid		   *uniqCollations pg_node_attr(array_size(uniqNumCols));
} Limit;


/*
 * RowMarkType -
 *	  enums for types of row-marking operations
 *
 * The first four of these values represent different lock strengths that
 * we can take on tuples according to SELECT FOR [KEY] UPDATE/SHARE requests.
 * We support these on regular tables, as well as on foreign tables whose FDWs
 * report support for late locking.  For other foreign tables, any locking
 * that might be done for such requests must happen during the initial row
 * fetch; their FDWs provide no mechanism for going back to lock a row later.
 * This means that the semantics will be a bit different than for a local
 * table; in particular we are likely to lock more rows than would be locked
 * locally, since remote rows will be locked even if they then fail
 * locally-checked restriction or join quals.  However, the prospect of
 * doing a separate remote query to lock each selected row is usually pretty
 * unappealing, so early locking remains a credible design choice for FDWs.
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we have to uniquely
 * identify all the source rows, not only those from the target relations, so
 * that we can perform EvalPlanQual rechecking at need.  For plain tables we
 * can just fetch the TID, much as for a target relation; this case is
 * represented by ROW_MARK_REFERENCE.  Otherwise (for example for VALUES or
 * FUNCTION scans) we have to copy the whole row value.  ROW_MARK_COPY is
 * pretty inefficient, since most of the time we'll never need the data; but
 * fortunately the overhead is usually not performance-critical in practice.
 * By default we use ROW_MARK_COPY for foreign tables, but if the FDW has
 * a concept of rowid it can request to use ROW_MARK_REFERENCE instead.
 * (Again, this probably doesn't make sense if a physical remote fetch is
 * needed, but for FDWs that map to local storage it might be credible.)
 */
typedef enum RowMarkType
{
	ROW_MARK_EXCLUSIVE,			/* 取得排他元组锁 */
	ROW_MARK_NOKEYEXCLUSIVE,	/* 取得无键排他元组锁 */
	ROW_MARK_SHARE,				/* 取得共享元组锁 */
	ROW_MARK_KEYSHARE,			/* 取得键共享元组锁 */
	ROW_MARK_REFERENCE,			/* 只取 TID,不加锁 */
	ROW_MARK_COPY				/* 物理拷贝行值 */
} RowMarkType;

#define RowMarkRequiresRowShareLock(marktype)  ((marktype) <= ROW_MARK_KEYSHARE)

/*
 * PlanRowMark -
 *	   plan-time representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we create a separate
 * PlanRowMark node for each non-target relation in the query.  Relations that
 * are not specified as FOR UPDATE/SHARE are marked ROW_MARK_REFERENCE (if
 * regular tables or supported foreign tables) or ROW_MARK_COPY (if not).
 *
 * Initially all PlanRowMarks have rti == prti and isParent == false.
 * When the planner discovers that a relation is the root of an inheritance
 * tree, it sets isParent true, and adds an additional PlanRowMark to the
 * list for each child relation (including the target rel itself in its role
 * as a child, if it is not a partitioned table).  Any non-leaf partitioned
 * child relations will also have entries with isParent = true.  The child
 * entries have rti == child rel's RT index and prti == top parent's RT index,
 * and can therefore be recognized as children by the fact that prti != rti.
 * The parent's allMarkTypes field gets the OR of (1<<markType) across all
 * its children (this definition allows children to use different markTypes).
 *
 * The planner also adds resjunk output columns to the plan that carry
 * information sufficient to identify the locked or fetched rows.  When
 * markType != ROW_MARK_COPY, these columns are named
 *		tableoid%u			OID of table
 *		ctid%u				TID of row
 * The tableoid column is only present for an inheritance hierarchy.
 * When markType == ROW_MARK_COPY, there is instead a single column named
 *		wholerow%u			whole-row value of relation
 * (An inheritance hierarchy could have all three resjunk output columns,
 * if some children use a different markType than others.)
 * In all three cases, %u represents the rowmark ID number (rowmarkId).
 * This number is unique within a plan tree, except that child relation
 * entries copy their parent's rowmarkId.  (Assigning unique numbers
 * means we needn't renumber rowmarkIds when flattening subqueries, which
 * would require finding and renaming the resjunk columns as well.)
 * Note this means that all tables in an inheritance hierarchy share the
 * same resjunk column names.
 */
typedef struct PlanRowMark
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	Index		rti;			/* 可标记关系的范围表下标 */
	Index		prti;			/* 父关系的范围表下标 */
	Index		rowmarkId;		/* resjunk 列的唯一标识符 */
	RowMarkType markType;		/* see enum above */
	int			allMarkTypes;	/* 所有子 (1<<markType) 的 OR */
	LockClauseStrength strength;	/* LockingClause 的强度,或 LCS_NONE */
	LockWaitPolicy waitPolicy;	/* NOWAIT 与 SKIP LOCKED 选项 */
	bool		isParent;		/* 若为"哑"父条目则为真 */
} PlanRowMark;


/*
 * 表示分区剪枝信息的节点类型。
 */

/*
 * PartitionPruneInfo - Details required to allow the executor to prune
 * partitions.
 *
 * Here we store mapping details to allow translation of a partitioned table's
 * index as returned by the partition pruning code into subplan indexes for
 * plan types which support arbitrary numbers of subplans, such as Append.
 * We also store various details to tell the executor when it should be
 * performing partition pruning.
 *
 * Each PartitionedRelPruneInfo describes the partitioning rules for a single
 * partitioned table (a/k/a level of partitioning).  Since a partitioning
 * hierarchy could contain multiple levels, we represent it by a List of
 * PartitionedRelPruneInfos, where the first entry represents the topmost
 * partitioned table and additional entries represent non-leaf child
 * partitions, ordered such that parents appear before their children.
 * Then, since an Append-type node could have multiple partitioning
 * hierarchies among its children, we have an unordered List of those Lists.
 *
 * prune_infos			List of Lists containing PartitionedRelPruneInfo nodes,
 *						one sublist per run-time-prunable partition hierarchy
 *						appearing in the parent plan node's subplans.
 * other_subplans		Indexes of any subplans that are not accounted for
 *						by any of the PartitionedRelPruneInfo nodes in
 *						"prune_infos".  These subplans must not be pruned.
 */
typedef struct PartitionPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	List	   *prune_infos;
	Bitmapset  *other_subplans;
} PartitionPruneInfo;

/*
 * PartitionedRelPruneInfo - Details required to allow the executor to prune
 * partitions for a single partitioned table.
 *
 * subplan_map[] and subpart_map[] are indexed by partition index of the
 * partitioned table referenced by 'rtindex', the partition index being the
 * order that the partitions are defined in the table's PartitionDesc.  For a
 * leaf partition p, subplan_map[p] contains the zero-based index of the
 * partition's subplan in the parent plan's subplan list; it is -1 if the
 * partition is non-leaf or has been pruned.  For a non-leaf partition p,
 * subpart_map[p] contains the zero-based index of that sub-partition's
 * PartitionedRelPruneInfo in the hierarchy's PartitionedRelPruneInfo list;
 * it is -1 if the partition is a leaf or has been pruned.  Note that subplan
 * indexes, as stored in 'subplan_map', are global across the parent plan
 * node, but partition indexes are valid only within a particular hierarchy.
 * relid_map[p] contains the partition's OID, or 0 if the partition was pruned.
 */
typedef struct PartitionedRelPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	/* 本层分区关系的 RT 下标 */
	Index		rtindex;

	/* 存在子计划或子分区的所有分区下标 */
	Bitmapset  *present_parts;

	/* 下列数组的长度: */
	int			nparts;

	/* 按分区下标子计划下标,或 -1 */
	int		   *subplan_map pg_node_attr(array_size(nparts));

	/* 按分区下标子分区下标,或 -1 */
	int		   *subpart_map pg_node_attr(array_size(nparts));

	/* 按分区下标的关系 OID,或 0 */
	Oid		   *relid_map pg_node_attr(array_size(nparts));

	/*
	 * initial_pruning_steps 说明执行器启动时如何剪枝(即不使用
	 * 任何 PARAM_EXEC Params);若不需启动剪枝则为 NIL。
	 * exec_pruning_steps 说明如何用 PARAM_EXEC Params 剪枝;若
	 * 不需每扫描剪枝则为 NIL。
	 */
	List	   *initial_pruning_steps;	/* PartitionPruneStep 列表 */
	List	   *exec_pruning_steps; /* PartitionPruneStep 列表 */

	/* All PARAM_EXEC Param IDs in exec_pruning_steps */
	Bitmapset  *execparamids;
} PartitionedRelPruneInfo;

/*
 * 分区剪枝步骤抽象 Node 类型(无此类型的具体 Node)。
 * 见上。
 *
 * step_id 是步骤在其剪枝上下文中的全局标识符。
 */
typedef struct PartitionPruneStep
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;
	int			step_id;
} PartitionPruneStep;

/*
 * PartitionPruneStepOp - Information to prune using a set of mutually ANDed
 *							OpExpr clauses
 *
 * This contains information extracted from up to partnatts OpExpr clauses,
 * where partnatts is the number of partition key columns.  'opstrategy' is the
 * strategy of the operator in the clause matched to the last partition key.
 * 'exprs' contains expressions which comprise the lookup key to be passed to
 * the partition bound search function.  'cmpfns' contains the OIDs of
 * comparison functions used to compare aforementioned expressions with
 * partition bounds.  Both 'exprs' and 'cmpfns' contain the same number of
 * items, up to partnatts items.
 *
 * Once we find the offset of a partition bound using the lookup key, we
 * determine which partitions to include in the result based on the value of
 * 'opstrategy'.  For example, if it were equality, we'd return just the
 * partition that would contain that key or a set of partitions if the key
 * didn't consist of all partitioning columns.  For non-equality strategies,
 * we'd need to include other partitions as appropriate.
 *
 * 'nullkeys' is the set containing the offset of the partition keys (0 to
 * partnatts - 1) that were matched to an IS NULL clause.  This is only
 * considered for hash partitioning as we need to pass which keys are null
 * to the hash partition bound search function.  It is never possible to
 * have an expression be present in 'exprs' for a given partition key and
 * the corresponding bit set in 'nullkeys'.
 */
typedef struct PartitionPruneStepOp
{
	PartitionPruneStep step;

	StrategyNumber opstrategy;
	List	   *exprs;
	List	   *cmpfns;
	Bitmapset  *nullkeys;
} PartitionPruneStepOp;

/*
 * PartitionPruneStepCombine - 用 BoolExpr 子句剪枝的信息
 *
 * 对 BoolExpr 子句,合并各参数子句所确定的分区集合。
 * 见上。
 */
typedef enum PartitionPruneCombineOp
{
	PARTPRUNE_COMBINE_UNION,
	PARTPRUNE_COMBINE_INTERSECT
} PartitionPruneCombineOp;

typedef struct PartitionPruneStepCombine
{
	PartitionPruneStep step;

	PartitionPruneCombineOp combineOp;
	List	   *source_stepids;
} PartitionPruneStepCombine;


/*
 * Plan invalidation info
 *
 * We track the objects on which a PlannedStmt depends in two ways:
 * relations are recorded as a simple list of OIDs, and everything else
 * is represented as a list of PlanInvalItems.  A PlanInvalItem is designed
 * to be used with the syscache invalidation mechanism, so it identifies a
 * system catalog entry by cache ID and hash value.
 */
typedef struct PlanInvalItem
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	int			cacheId;		/* syscache ID,见 utils/syscache.h */
	uint32		hashValue;		/* 对象缓存查找键的哈希值 */
} PlanInvalItem;

/*
 * MonotonicFunction
 *
 * Allows the planner to track monotonic properties of functions.  A function
 * is monotonically increasing if a subsequent call cannot yield a lower value
 * than the previous call.  A monotonically decreasing function cannot yield a
 * higher value on subsequent calls, and a function which is both must return
 * the same value on each call.
 */
typedef enum MonotonicFunction
{
	MONOTONICFUNC_NONE = 0,
	MONOTONICFUNC_INCREASING = (1 << 0),
	MONOTONICFUNC_DECREASING = (1 << 1),
	MONOTONICFUNC_BOTH = MONOTONICFUNC_INCREASING | MONOTONICFUNC_DECREASING
} MonotonicFunction;

#endif							/* PLANNODES_H */
