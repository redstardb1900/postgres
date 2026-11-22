/*-------------------------------------------------------------------------
 *
 * execnodes.h
 *	  definitions for executor state nodes
 *
 * Most plan node types declared in plannodes.h have a corresponding
 * execution-state node type declared here.  An exception is that
 * expression nodes (subtypes of Expr) are usually represented by steps
 * of an ExprState, and fully handled within execExpr* - but sometimes
 * their state needs to be shared with other parts of the executor, as
 * for example with SubPlanState, which nodeSubplan.c has to modify.
 *
 * Node types declared in this file do not have any copy/equal/out/read
 * support.  (That is currently hard-wired in gen_node_support.pl, rather
 * than being explicitly represented by pg_node_attr decorations here.)
 * There is no need for copy, equal, or read support for executor trees.
 * Output support could be useful for debugging; but there are a lot of
 * specialized fields that would require custom code, so for now it's
 * not provided.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/execnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#include "access/tupconvert.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "lib/ilist.h"
#include "lib/pairingheap.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "partitioning/partdefs.h"
#include "storage/condition_variable.h"
#include "utils/hsearch.h"
#include "utils/queryenvironment.h"
#include "utils/reltrigger.h"
#include "utils/sharedtuplestore.h"
#include "utils/snapshot.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"

struct PlanState;				/* 本文件中的前向引用 */
struct ParallelHashJoinState;
struct ExecRowMark;
struct ExprState;
struct ExprContext;
struct RangeTblEntry;			/* 避免在此包含 parsenodes.h */
struct ExprEvalStep;			/* 避免处处包含 execExpr.h */
struct CopyMultiInsertBuffer;
struct LogicalTapeSet;


/* ----------------
 *	ExprState 点
 *
 * ExprState 表示整棵表达式树的求值状态。
 * 含求值表达式的指令(在 ->steps 中)。
 * ----------------
 */
typedef Datum (*ExprStateEvalFunc) (struct ExprState *expression,
									struct ExprContext *econtext,
									bool *isNull);

/* ExprState->flags 中的位(私有标志位另见 execExpr.h): */
/* 表达式供 ExecQual() 使用 */
#define EEO_FLAG_IS_QUAL					(1 << 0)

typedef struct ExprState
{
	NodeTag		type;

	uint8		flags;			/* EEO_FLAG_* 位掩码,见上 */

	/*
	 * 标量表达式结果值的存储,或 ExecBuildProjectionInfo()
	 * 所建表达式中各列结果的存储。
	 */
#define FIELDNO_EXPRSTATE_RESNULL 2
	bool		resnull;
#define FIELDNO_EXPRSTATE_RESVALUE 3
	Datum		resvalue;

	/*
	 * 若投影元组结果,本槽保存结果;否则为 NULL。
	 */
#define FIELDNO_EXPRSTATE_RESULTSLOT 4
	TupleTableSlot *resultslot;

	/*
	 * 计算表达式返回值的指令。
	 */
	struct ExprEvalStep *steps;

	/*
	 * Function that actually evaluates the expression.  This can be set to
	 * different values depending on the complexity of the expression.
	 */
	ExprStateEvalFunc evalfunc;

	/* 原表达式树,仅供调试 */
	Expr	   *expr;

	/* evalfunc 的私有状态 */
	void	   *evalfunc_private;

	/*
	 * XXX: 下列字段仅在"编译"(ExecInitExpr)期间需要;
	 * 之后可丢弃。
	 */

	int			steps_len;		/* 当前步骤数 */
	int			steps_alloc;	/* steps 数组已分配长度 */

#define FIELDNO_EXPRSTATE_PARENT 11
	struct PlanState *parent;	/* 父 PlanState 节点(若有) */
	ParamListInfo ext_params;	/* 供编译 PARAM_EXTERN 节点 */

	Datum	   *innermost_caseval;
	bool	   *innermost_casenull;

	Datum	   *innermost_domainval;
	bool	   *innermost_domainnull;
} ExprState;


/* ----------------
 *	IndexInfo 信息
 *
 *	本结构保存为特定索引构造新索引项所需信息。
 *	用于 index_build 与零售式创建索引项。
 *	见上。
 *
 *	NumIndexAttrs 本索引总列数
 *	NumIndexKeyAttrs 索引键列数
 *	IndexAttrNumbers 用作键的底层关系属性号
 *	(零表示表达式)。也含
 * 关于 INCLUDE 列的信息。
 *	Expressions 表达式项的表达式树,无则为 NIL
 *	ExpressionsState 表达式的执行状态,无则为 NIL
 *	Predicate 部分索引谓词,无则为 NIL
 *	PredicateState 谓词的执行状态,无则为 NIL
 *	ExclusionOps 逐列排他运算符,无则为 NULL
 *	ExclusionProcs ExclusionOps 的底层函数 OID
 *	ExclusionStrats ExclusionOps 的 opclass 策略号
 *	UniqueOps 类似 Exclusion*,但用于唯一索引
 *	UniqueProcs
 *	UniqueStrats
 *	OpclassOptions opclass 专用选项,无则为 NULL
 *	Unique 是否唯一索引?
 *	NullsNotDistinct 否 NULLS NOT DISTINCT?
 *	ReadyForInserts 是否对插入有效?
 *	CheckedUnchanged IndexUnchanged 状态已判定?
 *	IndexUnchanged aminsert 提示,为零售插入缓存
 *	Concurrent 是否在做并发索引构建?
 *	BrokenHotChain 是否检测到断裂的 HOT 链?
 *	Summarizing 是否汇总索引?
 *	ParallelWorkers 请求的工作者数(不含领导者)
 *	Am 索引 AM 的 Oid
 *	AmCache 索引 AM 的私有缓存区
 *	Context 持有本 IndexInfo 的内存上下文
 *
 * ii_Concurrent、ii_BrokenHotChain 与 ii_ParallelWorkers
 * 仅在索引构建时使用;其他情况按惯例清零。
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumIndexAttrs;	/* 索引总列数 */
	int			ii_NumIndexKeyAttrs;	/* 索引键列数 */
	AttrNumber	ii_IndexAttrNumbers[INDEX_MAX_KEYS];
	List	   *ii_Expressions; /* Expr 列表 */
	List	   *ii_ExpressionsState;	/* ExprState 列表 */
	List	   *ii_Predicate;	/* Expr 列表 */
	ExprState  *ii_PredicateState;
	Oid		   *ii_ExclusionOps;	/* 每列一项的数组 */
	Oid		   *ii_ExclusionProcs;	/* 每列一项的数组 */
	uint16	   *ii_ExclusionStrats; /* 每列一项的数组 */
	Oid		   *ii_UniqueOps;	/* 每列一项的数组 */
	Oid		   *ii_UniqueProcs; /* 每列一项的数组 */
	uint16	   *ii_UniqueStrats;	/* 每列一项的数组 */
	Datum	   *ii_OpclassOptions;	/* 每列一项的数组 */
	bool		ii_Unique;
	bool		ii_NullsNotDistinct;
	bool		ii_ReadyForInserts;
	bool		ii_CheckedUnchanged;
	bool		ii_IndexUnchanged;
	bool		ii_Concurrent;
	bool		ii_BrokenHotChain;
	bool		ii_Summarizing;
	int			ii_ParallelWorkers;
	Oid			ii_Am;
	void	   *ii_AmCache;
	MemoryContext ii_Context;
} IndexInfo;

/* ----------------
 *	ExprContext_CB
 *
 *	ExprContext 关闭时要调用的回调列表。
 * ----------------
 */
typedef void (*ExprContextCallbackFunction) (Datum arg);

typedef struct ExprContext_CB
{
	struct ExprContext_CB *next;
	ExprContextCallbackFunction function;
	Datum		arg;
} ExprContext_CB;

/* ----------------
 *	  ExprContext
 *
 *		This class holds the "current context" information
 *		needed to evaluate expressions for doing tuple qualifications
 *		and tuple projections.  For example, if an expression refers
 *		to an attribute in the current inner tuple then we need to know
 *		what the current inner tuple is and so we look at the expression
 *		context.
 *
 *	There are two memory contexts associated with an ExprContext:
 *	* ecxt_per_query_memory is a query-lifespan context, typically the same
 *	  context the ExprContext node itself is allocated in.  This context
 *	  can be used for purposes such as storing function call cache info.
 *	* ecxt_per_tuple_memory is a short-term context for expression results.
 *	  As the name suggests, it will typically be reset once per tuple,
 *	  before we begin to evaluate expressions for that tuple.  Each
 *	  ExprContext normally has its very own per-tuple memory context.
 *
 *	CurrentMemoryContext should be set to ecxt_per_tuple_memory before
 *	calling ExecEvalExpr() --- see ExecEvalExprSwitchContext().
 * ----------------
 */
typedef struct ExprContext
{
	NodeTag		type;

	/* 表达式中 Var 节点可能引用的元组 */
#define FIELDNO_EXPRCONTEXT_SCANTUPLE 1
	TupleTableSlot *ecxt_scantuple;
#define FIELDNO_EXPRCONTEXT_INNERTUPLE 2
	TupleTableSlot *ecxt_innertuple;
#define FIELDNO_EXPRCONTEXT_OUTERTUPLE 3
	TupleTableSlot *ecxt_outertuple;

	/* 表达式求值的内存上下文 — 见上方注释 */
	MemoryContext ecxt_per_query_memory;
	MemoryContext ecxt_per_tuple_memory;

	/* 替换表达式中 Param 节点的值 */
	ParamExecData *ecxt_param_exec_vals;	/* 供 PARAM_EXEC 参数 */
	ParamListInfo ecxt_param_list_info; /* 供其他参数类型 */

	/*
	 * 替换 Agg 节点表达式中 Aggref 节点的值,或 WindowAgg
	 * 节点中 WindowFunc 节点的值。
	 */
#define FIELDNO_EXPRCONTEXT_AGGVALUES 8
	Datum	   *ecxt_aggvalues; /* 聚合/窗口函数的预计算值 */
#define FIELDNO_EXPRCONTEXT_AGGNULLS 9
	bool	   *ecxt_aggnulls;	/* 聚合/窗口函数的空标志 */

	/* 替换表达式中 CaseTestExpr 节点的值 */
#define FIELDNO_EXPRCONTEXT_CASEDATUM 10
	Datum		caseValue_datum;
#define FIELDNO_EXPRCONTEXT_CASENULL 11
	bool		caseValue_isNull;

	/* 替换表达式中 CoerceToDomainValue 节点的值 */
#define FIELDNO_EXPRCONTEXT_DOMAINDATUM 12
	Datum		domainValue_datum;
#define FIELDNO_EXPRCONTEXT_DOMAINNULL 13
	bool		domainValue_isNull;

	/* 链到含有的 EState(独立 ExprContext 则为 NULL) */
	struct EState *ecxt_estate;

	/* ExprContext 关闭或重扫时要回调的函数 */
	ExprContext_CB *ecxt_callbacks;
} ExprContext;

/*
 * Set-result status used when evaluating functions potentially returning a
 * set.
 */
typedef enum
{
	ExprSingleResult,			/* 表达式不返回集合 */
	ExprMultipleResult,			/* 本结果是集合的一个元素 */
	ExprEndResult				/* 集合中不再有元素 */
} ExprDoneCond;

/*
 * 返回集合函数的返回模式.注值须选为独立位以便形成
 * 位掩码指示支持的模式。SFRM_Materialize_Random 与
 * SFRM_Materialize_Preferred 是关于 SFRM_Materialize 模式的
 * 辅助标志,而非独立模式。
 */
typedef enum
{
	SFRM_ValuePerCall = 0x01,	/* 每次调用返回一个值 */
	SFRM_Materialize = 0x02,	/* 结果集实例化于 Tuplestore */
	SFRM_Materialize_Random = 0x04, /* Tuplestore needs randomAccess */
	SFRM_Materialize_Preferred = 0x08	/* caller prefers Tuplestore */
} SetFunctionReturnMode;

/*
 * When calling a function that might return a set (multiple rows),
 * a node of this type is passed as fcinfo->resultinfo to allow
 * return status to be passed back.  A function returning set should
 * raise an error if no such resultinfo is provided.
 */
typedef struct ReturnSetInfo
{
	NodeTag		type;
	/* 调用者设置值: */
	ExprContext *econtext;		/* 函数被调用所处的上下文 */
	TupleDesc	expectedDesc;	/* 调用者期望的元组描述符 */
	int			allowedModes;	/* bitmask: return modes caller can handle */
	/* 函数的结果状态(但由调用者预初始化): */
	SetFunctionReturnMode returnMode;	/* 实际返回模式 */
	ExprDoneCond isDone;		/* ValuePerCall 模式的状态 */
	/* Materialize 返回模式下函数填写的字段: */
	Tuplestorestate *setResult; /* 持有完整返回元组集 */
	TupleDesc	setDesc;		/* 返回元组的实际描述符 */
} ReturnSetInfo;

/* ----------------
 *	ProjectionInfo 节点信息
 *
 *	这是执行投影所需的全部信息 —
 *	即通过求值目标列表表达式形成新元组。
 *	需要做投影的节点创建一个。
 *
 *	目标元组槽保存在 ProjectionInfo->pi_state.resultslot。
 *	ExecProject() 求值 tlist、形成元组并存入给定槽.注
 *	除非随后调用 ExecMaterializeSlot() 强制转为物理元组,
 *	结果将是"虚拟"元组.该槽 tupledesc 必须匹配 tlist
 *	的输出!
 *	见上。
 * ----------------
 */
typedef struct ProjectionInfo
{
	NodeTag		type;
	/* 求值投影的指令 */
	ExprState	pi_state;
	/* 求值表达式所用的表达式上下文 */
	ExprContext *pi_exprContext;
} ProjectionInfo;

/* ----------------
 *	  JunkFilter
 *
 *	  This class is used to store information regarding junk attributes.
 *	  A junk attribute is an attribute in a tuple that is needed only for
 *	  storing intermediate information in the executor, and does not belong
 *	  in emitted tuples.  For example, when we do an UPDATE query,
 *	  the planner adds a "junk" entry to the targetlist so that the tuples
 *	  returned to ExecutePlan() contain an extra attribute: the ctid of
 *	  the tuple to be updated.  This is needed to do the update, but we
 *	  don't want the ctid to be part of the stored new tuple!  So, we
 *	  apply a "junk filter" to remove the junk attributes and form the
 *	  real output tuple.  The junkfilter code also provides routines to
 *	  extract the values of the junk attribute(s) from the input tuple.
 *
 *	  targetList:		the original target list (including junk attributes).
 *	  cleanTupType:		the tuple descriptor for the "clean" tuple (with
 *						junk attributes removed).
 *	  cleanMap:			A map with the correspondence between the non-junk
 *						attribute numbers of the "original" tuple and the
 *						attribute numbers of the "clean" tuple.
 *	  resultSlot:		tuple slot used to hold cleaned tuple.
 * ----------------
 */
typedef struct JunkFilter
{
	NodeTag		type;
	List	   *jf_targetList;
	TupleDesc	jf_cleanTupType;
	AttrNumber *jf_cleanMap;
	TupleTableSlot *jf_resultSlot;
} JunkFilter;

/*
 * OnConflictSetState
 *
 * ON CONFLICT DO UPDATE 操作的执行器状态。
 */
typedef struct OnConflictSetState
{
	NodeTag		type;

	TupleTableSlot *oc_Existing;	/* 存已有目标元组的槽 */
	TupleTableSlot *oc_ProjSlot;	/* CONFLICT ... SET ... 投影目标 */
	ProjectionInfo *oc_ProjInfo;	/* 供 ON CONFLICT DO UPDATE SET */
	ExprState  *oc_WhereClause; /* WHERE 子句的状态 */
} OnConflictSetState;

/* ----------------
 *	MergeActionState 信息
 *
 *	MERGE 动作的执行器状态。
 * ----------------
 */
typedef struct MergeActionState
{
	NodeTag		type;

	MergeAction *mas_action;	/* 关联 MergeAction 节点 */
	ProjectionInfo *mas_proj;	/* 本关系上动作目标列表的
								 * 投影 */
	ExprState  *mas_whenqual;	/* WHEN [NOT] MATCHED AND 条件 */
} MergeActionState;

/*
 * ResultRelInfo
 *
 * 每当更新已有关系时,须更新其上的索引,或许还要触发
 * 触发器。ResultRelInfo 保存关于结果关系全部所需信息,
 * 包括索引。
 *
 * 通常 ResultRelInfo 引用查询范围表中的表;则
 * ri_RangeTableIndex 是 RT 下标,ri_RelationDesc 只是相关
 * es_relations[] 项拷贝.但有些情形我为不在范围表中的
 * 关系创建 ResultRelInfo,即分区表元组路由目标,以及在
 * 非目标表上激发触发器时(见 ExecGetTriggerResultRel)。
 * 这些情形 ri_RangeTableIndex 为 0,ri_RelationDesc 是需
 * 单独关闭的单独打开的 relcache 指针。
 * 见上。
 * 见上。
 */
typedef struct ResultRelInfo
{
	NodeTag		type;

	/* 结果关系的范围表下标,若不在范围表中则为 0 */
	Index		ri_RangeTableIndex;

	/* 结果关系的关系描述符 */
	Relation	ri_RelationDesc;

	/* 结果关系上现有索引数 */
	int			ri_NumIndices;

	/* 索引的关系描述符数组 */
	RelationPtr ri_IndexRelationDescs;

	/* 索引的键/属性信息数组 */
	IndexInfo **ri_IndexRelationInfo;

	/*
	 * 对 UPDATE/DELETE 结果关系,源计划输出元组中行标识
	 * junk 属性的属性号
	 */
	AttrNumber	ri_RowIdAttNo;

	/* 对 UPDATE,要计算的生成列的 attnums */
	Bitmapset  *ri_extraUpdatedCols;

	/* INSERT/UPDATE 中生成新元组的投影 */
	ProjectionInfo *ri_projectNew;
	/* 保存该元组的槽 */
	TupleTableSlot *ri_newTupleSlot;
	/* 保存正被更新的旧元组的槽 */
	TupleTableSlot *ri_oldTupleSlot;
	/* 上方投影与槽是否已初始化? */
	bool		ri_projectNewInfoValid;

	/* 更新在读旧元组前做 LockTuple();见 README.tuplock */
	bool		ri_needLockTagTuple;

	/* 要激发触发器(若有) */
	TriggerDesc *ri_TrigDesc;

	/* 触发器函数的缓存查找信息 */
	FmgrInfo   *ri_TrigFunctions;

	/* array of trigger WHEN expr states */
	ExprState **ri_TrigWhenExprs;

	/* 触发器的可选运行时度量 */
	Instrumentation *ri_TrigInstrument;

	/* 为触发器/RETURNING 处理按需创建的槽 */
	TupleTableSlot *ri_ReturningSlot;	/* 供触发器输出元组 */
	TupleTableSlot *ri_TrigOldSlot; /* 供触发器的旧元组 */
	TupleTableSlot *ri_TrigNewSlot; /* 供触发器的新元组 */

	/* FDW 回调函数(若为外表) */
	struct FdwRoutine *ri_FdwRoutine;

	/* 可供保存 FDW 私有状态 */
	void	   *ri_FdwState;

	/* 直接修改外表时为真 */
	bool		ri_usesFdwDirectModify;

	/* 批量插入相关 */
	int			ri_NumSlots;	/* 数组中的槽数 */
	int			ri_NumSlotsInitialized; /* 已初始化的槽数 */
	int			ri_BatchSize;	/* 单批插入的最大槽数 */
	TupleTableSlot **ri_Slots;	/* 批量插入的输入元组 */
	TupleTableSlot **ri_PlanSlots;

	/* 要检查的 WithCheckOption 列表 */
	List	   *ri_WithCheckOptions;

	/* WithCheckOption 表达式状态表 */
	List	   *ri_WithCheckOptionExprs;

	/* 约束检查表达式状态数组 */
	ExprState **ri_ConstraintExprs;

	/* INSERT 与 UPDATE 的存储生成列表达式状态数组 */
	ExprState **ri_GeneratedExprsI;
	ExprState **ri_GeneratedExprsU;

	/* 需要计算的存储生成列数 */
	int			ri_NumGeneratedNeededI;
	int			ri_NumGeneratedNeededU;

	/* RETURNING 表达式列表 */
	List	   *ri_returningList;

	/* 用于计算 RETURNING 列表 */
	ProjectionInfo *ri_projectReturning;

	/* 用于检查冲突的仲裁索引列表 */
	List	   *ri_onConflictArbiterIndexes;

	/* ON CONFLICT 求值状态 */
	OnConflictSetState *ri_onConflict;

	/* 对 MERGE,MergeActionState 列表 */
	List	   *ri_matchedMergeAction;
	List	   *ri_notMatchedMergeAction;

	/* 分区检查表达式状态(尚未建立则为 NULL) */
	ExprState  *ri_PartitionCheckExpr;

	/*
	 * 把子结果关系元组转为查询中实际提到的表("根")格式
	 * 映射.仅在需要时计算。NULL 映射值为无需转换,故
	 * 须有单独标志显示映射是否已计算。
	 * 见上。
	 */
	TupleConversionMap *ri_ChildToRootMap;
	bool		ri_ChildToRootMapValid;

	/*
	 * 同上,但方向相反。
	 */
	TupleConversionMap *ri_RootToChildMap;
	bool		ri_RootToChildMapValid;

	/*
	 * 子结果关系所需的其他信息
	 *
	 * ri_RootResultRelInfo 给出查询中提到的目标关系。
	 * 用作元组路由和/或转移捕获的根。
	 *
	 * 若关系是要路由元组进入分区且需要 ri_RootToChildMap
	 * 转换,则 ri_PartitionTupleSlot 非 NULL。
	 */
	struct ResultRelInfo *ri_RootResultRelInfo;
	TupleTableSlot *ri_PartitionTupleSlot;

	/* 供 copyfrom.c 做多插入时使用 */
	struct CopyMultiInsertBuffer *ri_CopyMultiInsertBuffer;

	/*
	 * 当叶子分区参与其某祖先的跨分区更新时使用;
	 * 见 ExecCrossPartitionUpdateForeignKey()。
	 */
	List	   *ri_ancestorResultRels;
} ResultRelInfo;

/* ----------------
 *	AsyncRequest
 *
 * 异步元组请求的状态。
 * ----------------
 */
typedef struct AsyncRequest
{
	struct PlanState *requestor;	/* 想要元组的节点 */
	struct PlanState *requestee;	/* 要从其取元组的节点 */
	int			request_index;	/* 请求者的暂存空间 */
	bool		callback_pending;	/* 需要回调 */
	bool		request_complete;	/* 请求完成,结果有效 */
	TupleTableSlot *result;		/* Result (NULL or an empty slot if no more
								 * tuples) */
} AsyncRequest;

/* ----------------
 *	EState 信息
 *
 * 一次执行器调用的工作状态
 * ----------------
 */
typedef struct EState
{
	NodeTag		type;

	/* 所有查询类型基本状态: */
	ScanDirection es_direction; /* 当前扫描方向 */
	Snapshot	es_snapshot;	/* time qual to use */
	Snapshot	es_crosscheck_snapshot; /* RI 交叉检查时间限定 */
	List	   *es_range_table; /* RangeTblEntry 列表 */
	Index		es_range_table_size;	/* 范围表数组的大小 */
	Relation   *es_relations;	/* Array of per-range-table-entry Relation
								 * pointers, or NULL if not yet opened */
	struct ExecRowMark **es_rowmarks;	/* 每范围表项的
										 * ExecRowMarks,无则为 NULL */
	List	   *es_rteperminfos;	/* RTEPermissionInfo 列表 */
	PlannedStmt *es_plannedstmt;	/* 链到计划树顶 */
	const char *es_sourceText;	/* 来自 QueryDesc 源文本 */

	JunkFilter *es_junkFilter;	/* 顶层 junk 过滤器(若有) */

	/* 若查询可插入/删除元组,用于标记它们的命令 ID */
	CommandId	es_output_cid;

	/* 插入/更新/删除查询的目标表信息: */
	ResultRelInfo **es_result_relations;	/* 每范围表项的
											 * ResultRelInfo 指针,若非
											 * 目标表则为 NULL */
	List	   *es_opened_result_relations; /* es_result_relations 中
											 * 非 NULL 项的列表,无
											 * 特定次序 */

	PartitionDirectory es_partition_directory;	/* 供 PartitionDesc 查找 */

	/*
	 * 下列列表含元组路由代码为 es_result_relations 数组中
	 * 找不到的分区创建的 ResultRelInfo。
	 */
	List	   *es_tuple_routing_result_relations;

	/* 激发触发器所用: */
	List	   *es_trig_target_relations;	/* 仅触发器 ResultRelInfos */

	/* 参数信息: */
	ParamListInfo es_param_list_info;	/* 外部参数的值 */
	ParamExecData *es_param_exec_vals;	/* 内部参数的值 */

	QueryEnvironment *es_queryEnv;	/* 查询环境 */

	/* 其他工作状态: */
	MemoryContext es_query_cxt; /* EState 所在的每查询上下文 */

	List	   *es_tupleTable;	/* TupleTableSlots 列表 */

	uint64		es_processed;	/* # of tuples processed during one
								 * ExecutorRun() call. */
	uint64		es_total_processed; /* 跨所有 ExecutorRun() 调用
									 * 累计总元组数. */

	int			es_top_eflags;	/* 传给 ExecutorStart  eflags */
	int			es_instrument;	/* InstrumentOption 标志 OR */
	bool		es_finished;	/* ExecutorFinish 完成则为真 */

	List	   *es_exprcontexts;	/* EState 内的 ExprContexts 列表 */

	List	   *es_subplanstates;	/* SubPlans 的 PlanState 列表 */

	List	   *es_auxmodifytables; /* 次要 ModifyTableStates 列表 */

	/*
	 * 本 ExprContext 供每输出元组操作,如约束检查与索引值
	 * 计算。每个输出元组会重置。注:仅在需要时创建。
	 * 见上。
	 */
	ExprContext *es_per_tuple_exprcontext;

	/*
	 * 若非 NULL,这是 EPQState 的 EState。作为 EState 字段是为
	 * 让 EvalPlanQual 感知的执行器节点能检测需做 EPQ 相关
	 * 工作,并提供必要信息。
	 * 见上。
	 */
	struct EPQState *es_epq_active;

	bool		es_use_parallel_mode;	/* 能否用并行工作者? */

	/* 并行执行要用的每查询共享内存区。 */
	struct dsa_area *es_query_dsa;

	/*
	 * JIT 信息。es_jit_flags 指示是否应做 JIT 及选项。
	 * es_jit 在真正做 JIT 时按需创建。
	 * 见上。
	 *
	 * es_jit_worker_instr 是所有工作者按需分配合并
	 * 插桩。领导者的插桩单独保存,由 ExplainPrintJITSummary()
	 * 按需合并。
	 */
	int			es_jit_flags;
	struct JitContext *es_jit;
	struct JitInstrumentation *es_jit_worker_instr;

	/*
	 * 要对之执行批量插入外表的 ResultRelInfo 表,以及拥有
	 * 的 ModifyTableStates,按相同次序存放。
	 */
	List	   *es_insert_pending_result_relations;
	List	   *es_insert_pending_modifytables;
} EState;


/*
 * ExecRowMark -
 *	   runtime representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR [KEY] UPDATE/SHARE, we will have
 * an ExecRowMark for each non-target relation in the query (except inheritance
 * parent RTEs, which can be ignored at runtime).  Virtual relations such as
 * subqueries-in-FROM will have an ExecRowMark with relation == NULL.  See
 * PlanRowMark for details about most of the fields.  In addition to fields
 * directly derived from PlanRowMark, we store an activity flag (to denote
 * inactive children of inheritance trees), curCtid, which is used by the
 * WHERE CURRENT OF code, and ermExtra, which is available for use by the plan
 * node that sources the relation (e.g., for a foreign table the FDW can use
 * ermExtra to hold information).
 *
 * EState->es_rowmarks is an array of these structs, indexed by RT index,
 * with NULLs for irrelevant RT indexes.  es_rowmarks itself is NULL if
 * there are no rowmarks.
 */
typedef struct ExecRowMark
{
	Relation	relation;		/* 已打开并适当加锁的关系 */
	Oid			relid;			/* 其 OID(子查询则为 InvalidOid) */
	Index		rti;			/* 其范围表下标 */
	Index		prti;			/* 父范围表下标(若为子) */
	Index		rowmarkId;		/* resjunk 列的唯一标识符 */
	RowMarkType markType;		/* see enum in nodes/plannodes.h */
	LockClauseStrength strength;	/* LockingClause 的强度,或 LCS_NONE */
	LockWaitPolicy waitPolicy;	/* NOWAIT 与 SKIP LOCKED */
	bool		ermActive;		/* 本标记对当前元组是否相关? */
	ItemPointerData curCtid;	/* 当前锁定元组的 ctid(若有) */
	void	   *ermExtra;		/* 可供关系源节点使用 */
} ExecRowMark;

/*
 * ExecAuxRowMark -
 *	FOR [KEY] UPDATE/SHARE 子句的额外运行时表示
 *
 * 每个 LockRows 与 ModifyTable 节点保持其需处理的 rowmarks
 * 列表。除指向 es_rowmarks 相关项指针外,本结构还携带与
 * rowmark 关联的 resjunk 列号(细节见 PlanRowMark 注释)。
 * 见上。
 */
typedef struct ExecAuxRowMark
{
	ExecRowMark *rowmark;		/* es_rowmarks 中的相关项 */
	AttrNumber	ctidAttNo;		/* resno of ctid junk attribute, if any */
	AttrNumber	toidAttNo;		/* resno of tableoid junk attribute, if any */
	AttrNumber	wholeAttNo;		/* 整行 junk 属性的 resno(若有) */
} ExecAuxRowMark;


/* ----------------------------------------------------------------
 *	元组哈希表
 *
 * 全内存元组哈希表用于多种目的。
 *
 * 注:tab_hash_funcs 用于表中存储的键数据类型,
 * tab_eq_funcs 是这些类型的非跨类型相等运算符。
 * 通常只用这些函数,但 FindTupleHashEntry() 支持用跨类型
 * 哈希搜索哈希表。为此调用者须提供 LHS 数据类型哈希
 * 函数以及要用的跨类型相等运算符。做此类搜索时
 * in_hash_funcs 与 cur_eq_func 指向调用者的函数数组。
 * LookupTupleHashEntry() 期间它们分别指向 tab_hash_funcs
 * 与 tab_eq_func。
 * 见上。
 * ----------------------------------------------------------------
 */
typedef struct TupleHashEntryData *TupleHashEntry;
typedef struct TupleHashTableData *TupleHashTable;

typedef struct TupleHashEntryData
{
	MinimalTuple firstTuple;	/* 本组中第一个元组的拷贝 */
	void	   *additional;		/* user data */
	uint32		status;			/* hash status */
	uint32		hash;			/* hash value (cached) */
} TupleHashEntryData;

/* 定义生成元组哈希表接口所需的参数 */
#define SH_PREFIX tuplehash
#define SH_ELEMENT_TYPE TupleHashEntryData
#define SH_KEY_TYPE MinimalTuple
#define SH_SCOPE extern
#define SH_DECLARE
#include "lib/simplehash.h"

typedef struct TupleHashTableData
{
	tuplehash_hash *hashtab;	/* 底层哈希表 */
	int			numCols;		/* 查找键中的列数 */
	AttrNumber *keyColIdx;		/* 键列的属性号 */
	FmgrInfo   *tab_hash_funcs; /* 表数据类型的哈希函数 */
	ExprState  *tab_eq_func;	/* 表数据类型的比较器 */
	Oid		   *tab_collations; /* 哈希与比较的排序规则 */
	MemoryContext tablecxt;		/* 含表的内存上下文 */
	MemoryContext tempcxt;		/* 函数求值的上下文 */
	Size		entrysize;		/* 使每个哈希项的实际大小 */
	TupleTableSlot *tableslot;	/* 引用表项的槽 */
	/* 下列字段为每次表搜索临时设置: */
	TupleTableSlot *inputslot;	/* 当前输入元组的槽 */
	FmgrInfo   *in_hash_funcs;	/* 输入数据类型的哈希函数 */
	ExprState  *cur_eq_func;	/* 输入相对表的比较器 */
	uint32		hash_iv;		/* 哈希函数 IV */
	ExprContext *exprcontext;	/* 表达式上下文 */
}			TupleHashTableData;

typedef tuplehash_iterator TupleHashIterator;

/*
 * 用 InitTupleHashIterator/TermTupleHashIterator 做读写扫描。
 * 若表可冻结则用 ResetTupleHashIterator(此情形无需显式
 * 扫描终止)。
 */
#define InitTupleHashIterator(htable, iter) \
	tuplehash_start_iterate(htable->hashtab, iter)
#define TermTupleHashIterator(iter) \
	((void) 0)
#define ResetTupleHashIterator(htable, iter) \
	InitTupleHashIterator(htable, iter)
#define ScanTupleHashTable(htable, iter) \
	tuplehash_iterate(htable->hashtab, iter)


/* ----------------------------------------------------------------
 *	表达式状态节点
 *
 * 从前,计划表达式树中每个点都有对应单独执行器表达式
 * 状态点。现已不然;对常见表达式节点类型,全部执行信息
 * 嵌入单个 ExprState 节点步骤中.但仍有少数执行器状态
 * 节点类型用于选定表达式节点类型,主要是信息须与执行
 * 状态树其他部分共享的那些。
 * 见上。
 * ----------------------------------------------------------------
 */

/* ----------------
 *	WindowFuncExprState 点
 * ----------------
 */
typedef struct WindowFuncExprState
{
	NodeTag		type;
	WindowFunc *wfunc;			/* 表达式计划点 */
	List	   *args;			/* 参数表达式的 ExprStates */
	ExprState  *aggfilter;		/* FILTER 表达式 */
	int			wfuncno;		/* wfunc 在其计划节点内的 ID 号 */
} WindowFuncExprState;


/* ----------------
 *	SetExprState 点
 *
 * 求值可能返回集合的表达式(如 FuncExpr 或 OpExpr)的状态。
 * 某些情形如 ROWS FROM(...) 中部分表达式可能不是 SRF,但仍
 * 用同一机制;将视为返回单行的 SRF。
 * 见上。
 * ----------------
 */
typedef struct SetExprState
{
	NodeTag		type;
	Expr	   *expr;			/* 表达式计划点 */
	List	   *args;			/* 参数表达式的 ExprStates */

	/*
	 * 在 ROWS FROM 中,函数可被内联,去掉通常在内的 FuncExpr。
	 * 此情形这是已编译表达式(不能返回集合),用常规
	 * ExecEvalExpr() 求值。
	 */
	ExprState  *elidedFuncState;

	/*
	 * 目标函数的函数管理器查找信息。若 func.fn_oid 为
	 * InvalidOid,则尚未初始化它(也未初始化下列字段,除
	 * funcReturnsSet 外)。
	 */
	FmgrInfo	func;

	/*
	 * For a set-returning function (SRF) that returns a tuplestore, we keep
	 * the tuplestore here and dole out the result rows one at a time. The
	 * slot holds the row currently being returned.
	 */
	Tuplestorestate *funcResultStore;
	TupleTableSlot *funcResultSlot;

	/*
	 * In some cases we need to compute a tuple descriptor for the function's
	 * output.  If so, it's stored here.
	 */
	TupleDesc	funcResultDesc;
	bool		funcReturnsTuple;	/* funcResultDesc 非 NULL 时有效 */

	/*
	 * 记住函数是否声明返回集合。由 ExecInitExpr 设置,即使
	 * FmgrInfo 尚未建立也有效。
	 */
	bool		funcReturnsSet;

	/*
	 * setArgsValid is true when we are evaluating a set-returning function
	 * that uses value-per-call mode and we are in the middle of a call
	 * series; we want to pass the same argument values to the function again
	 * (and again, until it returns ExprEndResult).  This indicates that
	 * fcinfo_data already contains valid argument data.
	 */
	bool		setArgsValid;

	/*
	 * 记住是否已为本 SetExprState 注册关闭回调。仅当
	 * funcResultStore 或 setArgsValid 至少设过一次才注册(因
	 * 回调只为释放 tuplestore 或清除 setArgsValid)。
	 * 见上。
	 */
	bool		shutdown_reg;	/* 已注册关闭回调 */

	/*
	 * 函数的调用参数结构。若 func.fn_oid 有效则已初始化
	 * (由 InitFunctionCallInfoData)。setArgsValid 为真时也在
	 * 调用间保存参数值。
	 */
	FunctionCallInfo fcinfo;
} SetExprState;

/* ----------------
 *	SubPlanState 点
 * ----------------
 */
typedef struct SubPlanState
{
	NodeTag		type;
	SubPlan    *subplan;		/* 表达式计划点 */
	struct PlanState *planstate;	/* 子选择计划的状态树 */
	struct PlanState *parent;	/* 父计划节点的状态树 */
	ExprState  *testexpr;		/* 组合表达式的状态 */
	List	   *args;			/* 参数表达式的状态 */
	HeapTuple	curTuple;		/* 来自子计划最近元组的拷贝 */
	Datum		curArray;		/* 来自 ARRAY() 子计划最近数组 */
	/* 对子选择输出做哈希时使用这些: */
	TupleDesc	descRight;		/* 投影后的子选择描述符 */
	ProjectionInfo *projLeft;	/* 供投影左侧表达式 */
	ProjectionInfo *projRight;	/* 供投影子选择输出 */
	TupleHashTable hashtable;	/* 无空子选择行的哈希表 */
	TupleHashTable hashnulls;	/* 含空值行的哈希表 */
	bool		havehashrows;	/* 哈希表非空则为真 */
	bool		havenullrows;	/* hashnulls 非空则为真 */
	MemoryContext hashtablecxt; /* 含哈希表的内存上下文 */
	MemoryContext hashtempcxt;	/* 哈希表的临时内存上下文 */
	ExprContext *innerecontext; /* 计算内元组的 econtext */
	int			numCols;		/* 正被哈希的列数 */
	/* 其余字段各为长度 numCols 的数组: */
	AttrNumber *keyColIdx;		/* 哈希表的控制数据 */
	Oid		   *tab_eq_funcoids;	/* 表数据类型的相等
									 * 函数 oid */
	Oid		   *tab_collations; /* 哈希与比较的排序规则 */
	FmgrInfo   *tab_hash_funcs; /* 表数据类型的哈希函数 */
	FmgrInfo   *tab_eq_funcs;	/* 表数据类型的相等函数 */
	FmgrInfo   *lhs_hash_funcs; /* 左侧数据类型的哈希函数 */
	FmgrInfo   *cur_eq_funcs;	/* LHS 相对表的相等函数 */
	ExprState  *cur_eq_comp;	/* LHS 相对表的相等比较器 */
} SubPlanState;

/*
 * DomainConstraintState - CoerceToDomain 期间要检查的一项
 *
 * 注:我们视其为 ExprState 树的一部分,故按 xxxState 约定
 * 命名。但没有直接关联的计划树节点。
 * 见上。
 */
typedef enum DomainConstraintType
{
	DOM_CONSTRAINT_NOTNULL,
	DOM_CONSTRAINT_CHECK
} DomainConstraintType;

typedef struct DomainConstraintState
{
	NodeTag		type;
	DomainConstraintType constrainttype;	/* 约束类型 */
	char	   *name;			/* 约束名(供错误消息) */
	Expr	   *check_expr;		/* 对 CHECK,布尔表达式 */
	ExprState  *check_exprstate;	/* check_expr 求值状态,或 NULL */
} DomainConstraintState;


/* ----------------------------------------------------------------
 *				 Executor State Trees
 *
 * An executing query has a PlanState tree paralleling the Plan tree
 * that describes the plan.
 * ----------------------------------------------------------------
 */

/* ----------------
 *	ExecProcNodeMtd
 *
 * 这 ExecProcNode 调用以从执行器点返回下一元组方法.
 * 若无更多元组则返回 NULL 或空 TupleTableSlot。
 * 见上。
 * ----------------
 */
typedef TupleTableSlot *(*ExecProcNodeMtd) (struct PlanState *pstate);

/* ----------------
 *	PlanState 点
 *
 * 我们从不真正实例化任何 PlanState 节点;这只是所有
 * PlanState 型节点的公共抽象超类。
 * ----------------
 */
typedef struct PlanState
{
	pg_node_attr(abstract)

	NodeTag		type;

	Plan	   *plan;			/* 关联 Plan 节点 */

	EState	   *state;			/* 执行时,各节点状态指向
								 * 整个顶层计划的一个
								 * EState */

	ExecProcNodeMtd ExecProcNode;	/* 返回下一元组的函数 */
	ExecProcNodeMtd ExecProcNodeReal;	/* 若上方是包装则为实际
										 * 函数 */

	Instrumentation *instrument;	/* 本节点的可选运行时统计 */
	WorkerInstrumentation *worker_instrument;	/* 每工作者插桩 */

	/* 每工作者 JIT 插桩 */
	struct SharedJitInstrumentation *worker_jit_instrument;

	/*
	 * Common structural data for all Plan types.  These links to subsidiary
	 * state trees parallel links in the associated plan tree (except for the
	 * subPlan list, which does not exist in the plan tree).
	 */
	ExprState  *qual;			/* 布尔限定条件 */
	struct PlanState *lefttree; /* 输入计划树 */
	struct PlanState *righttree;

	List	   *initPlan;		/* Init SubPlanState 节点(非相关表达式
								 * 子查询) */
	List	   *subPlan;		/* 我表达式中的 SubPlanState 点 */

	/*
	 * 参数变化驱动重扫的管理状态
	 */
	Bitmapset  *chgParam;		/* 已变 Params 的 ID 集合 */

	/*
	 * 多数(若非全部)点类型所需其他运行时状态.
	 */
	TupleDesc	ps_ResultTupleDesc; /* 节点返回类型 */
	TupleTableSlot *ps_ResultTupleSlot; /* 我的结果元组的槽 */
	ExprContext *ps_ExprContext;	/* 节点的表达式求值上下文 */
	ProjectionInfo *ps_ProjInfo;	/* 做元组投影的信息 */

	bool		async_capable;	/* true if node is async-capable */

	/*
	 * Scanslot's descriptor if known. This is a bit of a hack, but otherwise
	 * it's hard for expression compilation to optimize based on the
	 * descriptor, without encoding knowledge about all executor nodes.
	 */
	TupleDesc	scandesc;

	/*
	 * Define the slot types for inner, outer and scanslots for expression
	 * contexts with this state as a parent.  If *opsset is set, then
	 * *opsfixed indicates whether *ops is guaranteed to be the type of slot
	 * used. That means that every slot in the corresponding
	 * ExprContext.ecxt_*tuple will point to a slot of that type, while
	 * evaluating the expression.  If *opsfixed is false, but *ops is set,
	 * that indicates the most likely type of slot.
	 *
	 * The scan* fields are set by ExecInitScanTupleSlot(). If that's not
	 * called, nodes can initialize the fields themselves.
	 *
	 * If outer/inneropsset is false, the information is inferred on-demand
	 * using ExecGetResultSlotOps() on ->righttree/lefttree, using the
	 * corresponding node's resultops* fields.
	 *
	 * The result* fields are automatically set when ExecInitResultSlot is
	 * used (be it directly or when the slot is created by
	 * ExecAssignScanProjectionInfo() /
	 * ExecConditionalAssignProjectionInfo()).  If no projection is necessary
	 * ExecConditionalAssignProjectionInfo() defaults those fields to the scan
	 * operations.
	 */
	const TupleTableSlotOps *scanops;
	const TupleTableSlotOps *outerops;
	const TupleTableSlotOps *innerops;
	const TupleTableSlotOps *resultops;
	bool		scanopsfixed;
	bool		outeropsfixed;
	bool		inneropsfixed;
	bool		resultopsfixed;
	bool		scanopsset;
	bool		outeropsset;
	bool		inneropsset;
	bool		resultopsset;
} PlanState;

/* ----------------
 *	these are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlanState(node)		(((PlanState *)(node))->righttree)
#define outerPlanState(node)		(((PlanState *)(node))->lefttree)

/* 内联访问某些插桩计数器的宏 */
#define InstrCountTuples2(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->ntuples2 += (delta); \
	} while (0)
#define InstrCountFiltered1(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->nfiltered1 += (delta); \
	} while(0)
#define InstrCountFiltered2(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->nfiltered2 += (delta); \
	} while(0)

/*
 * EPQState is state for executing an EvalPlanQual recheck on a candidate
 * tuples e.g. in ModifyTable or LockRows.
 *
 * To execute EPQ a separate EState is created (stored in ->recheckestate),
 * which shares some resources, like the rangetable, with the main query's
 * EState (stored in ->parentestate). The (sub-)tree of the plan that needs to
 * be rechecked (in ->plan), is separately initialized (into
 * ->recheckplanstate), but shares plan nodes with the corresponding nodes in
 * the main query. The scan nodes in that separate executor tree are changed
 * to return only the current tuple of interest for the respective
 * table. Those tuples are either provided by the caller (using
 * EvalPlanQualSlot), and/or found using the rowmark mechanism (non-locking
 * rowmarks by the EPQ machinery itself, locking ones by the caller).
 *
 * While the plan to be checked may be changed using EvalPlanQualSetPlan(),
 * all such plans need to share the same EState.
 */
typedef struct EPQState
{
	/* 由 EvalPlanQualInit() 初始化且之后不变: */
	EState	   *parentestate;	/* 主查询的 EState */
	int			epqParam;		/* 强制扫描点重求值 Param 的 ID */
	List	   *resultRelations;	/* RT 下标整数列表,或 NIL */

	/*
	 * relsubs_slot[scanrelid-1] 保存扫描节点为第 scanrelid 个
	 * RT 下标返回 EPQ 测试元组,以代替实际表扫描.调用者应
	 * 用 EvalPlanQualSlot() 取这些槽。
	 * 见上。
	 */
	List	   *tuple_table;	/* relsubs_slot 的元组表 */
	TupleTableSlot **relsubs_slot;

	/*
	 * Initialized by EvalPlanQualInit(), may be changed later with
	 * EvalPlanQualSetPlan():
	 */

	Plan	   *plan;			/* 要执行的计划树 */
	List	   *arowMarks;		/* ExecAuxRowMarks(仅非锁定) */


	/*
	 * The original output tuple to be rechecked.  Set by
	 * EvalPlanQualSetSlot(), before EvalPlanQualNext() or EvalPlanQual() may
	 * be called.
	 */
	TupleTableSlot *origslot;


	/* 由 EvalPlanQualBegin() 初始化或重置: */

	EState	   *recheckestate;	/* EPQ 执行的 EState,见上 */

	/*
	 * 可用 EvalPlanQualFetchRowMark() 按需取
	 * rowmarks,以 scanrelid-1 索引。仅非锁定
	 * rowmarks.
	 */
	ExecAuxRowMark **relsubs_rowmark;

	/*
	 * relsubs_done[scanrelid - 1] is true if there is no EPQ tuple for this
	 * target relation or it has already been fetched in the current scan of
	 * this target relation within the current EvalPlanQual test.
	 */
	bool	   *relsubs_done;

	/*
	 * relsubs_blocked[scanrelid - 1] is true if there is no EPQ tuple for
	 * this target relation during the current EvalPlanQual test.  We keep
	 * these flags set for all relids listed in resultRelations, but
	 * transiently clear the one for the relation whose tuple is actually
	 * passed to EvalPlanQual().
	 */
	bool	   *relsubs_blocked;

	PlanState  *recheckplanstate;	/* 供 ->plan  EPQ 专用执行节点 */
} EPQState;


/* ----------------
 *	ResultState 信息
 * ----------------
 */
typedef struct ResultState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	ExprState  *resconstantqual;
	bool		rs_done;		/* are we done? */
	bool		rs_checkqual;	/* 是否需要检查限定? */
} ResultState;

/* ----------------
 *	ProjectSetState 信息
 *
 * 注:至少有一个 elems 将是 SetExprState;其余是常规
 * ExprStates。
 * ----------------
 */
typedef struct ProjectSetState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	Node	  **elems;			/* 表达式状态数组 */
	ExprDoneCond *elemdone;		/* 每 SRF 的完成状态数组 */
	int			nelems;			/* elemdone[] 数组的长度 */
	bool		pending_srf_tuples; /* 仍在求值 tlist 中的 srfs? */
	MemoryContext argcontext;	/* SRF 参数的上下文 */
} ProjectSetState;


/* mt_merge_subcommands 的标志 */
#define MERGE_INSERT	0x01
#define MERGE_UPDATE	0x02
#define MERGE_DELETE	0x04

/* ----------------
 *	ModifyTableState 信息
 * ----------------
 */
typedef struct ModifyTableState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	CmdType		operation;		/* INSERT,UPDATE、DELETE 或MERGE */
	bool		canSetTag;		/* 是否设置命令标签/es_processed? */
	bool		mt_done;		/* are we done? */
	int			mt_nrels;		/* resultRelInfo[] 中的项数 */
	ResultRelInfo *resultRelInfo;	/* 关于目标关系的信息 */

	/*
	 * Target relation mentioned in the original statement, used to fire
	 * statement-level triggers and as the root for tuple routing.  (This
	 * might point to one of the resultRelInfo[] entries, but it can also be a
	 * distinct struct.)
	 */
	ResultRelInfo *rootResultRelInfo;

	EPQState	mt_epqstate;	/* 供求值 EvalPlanQual 复核 */
	bool		fireBSTriggers; /* 否需要激发语句触发器? */

	/*
	 * These fields are used for inherited UPDATE and DELETE, to track which
	 * target relation a given tuple is from.  If there are a lot of target
	 * relations, we use a hash table to translate table OIDs to
	 * resultRelInfo[] indexes; otherwise mt_resultOidHash is NULL.
	 */
	int			mt_resultOidAttno;	/* resno of "tableoid" junk attr */
	Oid			mt_lastResultOid;	/* tableoid 的最近所见值 */
	int			mt_lastResultIndex; /* resultRelInfo[] 中的对应下标 */
	HTAB	   *mt_resultOidHash;	/* 加速查找的可选哈希表 */

	/*
	 * 分区表 UPDATE 期间以根分区表行类型存元组的槽。
	 * 见上。
	 */
	TupleTableSlot *mt_root_tuple_slot;

	/* 元组路由支持信息 */
	struct PartitionTupleRouting *mt_partition_tuple_routing;

	/* 控制指定操作的转移表填充 */
	struct TransitionCaptureState *mt_transition_capture;

	/* 控制 INSERT...ON CONFLICT UPDATE 的转移表填充 */
	struct TransitionCaptureState *mt_oc_transition_capture;

	/* 显示存在哪些子命令 INS/UPD/DEL/DO NOTHING 的标志 */
	int			mt_merge_subcommands;

	/* MERGE 的元组计数器 */
	double		mt_merge_inserted;
	double		mt_merge_updated;
	double		mt_merge_deleted;
} ModifyTableState;

/* ----------------
 *	AppendState 信息
 *
 *	nplans 数组中有多少计划
 *	whichplan 正在执行哪个同步计划(0..n-1)
 *	或特殊负值。见 nodeAppend.c。
 *	prune_state 允许从扫描中消除分区所需细节,
 *	不可能则为 NULL。
 *	valid_subplans 运行时剪枝时,有效同步 appendplans
 *	要扫描的下标。
 * ----------------
 */

struct AppendState;
typedef struct AppendState AppendState;
struct ParallelAppendState;
typedef struct ParallelAppendState ParallelAppendState;
struct PartitionPruneState;

struct AppendState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	PlanState **appendplans;	/* 我的输入的 PlanStates 数组 */
	int			as_nplans;
	int			as_whichplan;
	bool		as_begun;		/* 假表示需要初始化 */
	Bitmapset  *as_asyncplans;	/* 异步计划下标 */
	int			as_nasyncplans; /* 异步计划数 */
	AsyncRequest **as_asyncrequests;	/* AsyncRequests 数组 */
	TupleTableSlot **as_asyncresults;	/* 异步计划未返回的结果 */
	int			as_nasyncresults;	/* as_asyncresults 中有效项数 */
	bool		as_syncdone;	/* 异步模式下全部同步计划
								 * 已完成则为真,否则假 */
	int			as_nasyncremain;	/* 剩余异步计划数 */
	Bitmapset  *as_needrequest; /* 需要新请求的异步计划 */
	struct WaitEventSet *as_eventset;	/* WaitEventSet used to configure file
										 * descriptor wait events */
	int			as_first_partial_plan;	/* 含第一个部分计划的
										 * 'appendplans' 下标 */
	ParallelAppendState *as_pstate; /* 并行协调信息 */
	Size		pstate_len;		/* 并行协调信息的大小 */
	struct PartitionPruneState *as_prune_state;
	bool		as_valid_subplans_identified;	/* is as_valid_subplans valid? */
	Bitmapset  *as_valid_subplans;
	Bitmapset  *as_valid_asyncplans;	/* 有效异步计划下标 */
	bool		(*choose_next_subplan) (AppendState *);
};

/* ----------------
 *	MergeAppendState 信息
 *
 *	nplans 数组中有多少计划
 *	nkeys 排序键列数
 *	sortkeys SortSupport 表示的排序键
 *	slots 各子计划当前输出元组
 *	heap 活动元组的堆
 *	initialized 是否已从各子计划取过第一元组
 *	prune_state 允许从扫描中消除分区所需细节,
 *	不可能则为 NULL。
 *	valid_subplans 运行时剪枝时,有效 mergeplans 要扫描
 *	下标。
 * ----------------
 */
typedef struct MergeAppendState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	PlanState **mergeplans;		/* 我的输入的 PlanStates 数组 */
	int			ms_nplans;
	int			ms_nkeys;
	SortSupport ms_sortkeys;	/* 长度 ms_nkeys 数组 */
	TupleTableSlot **ms_slots;	/* 长度 ms_nplans 数组 */
	struct binaryheap *ms_heap; /* 槽下标的二叉堆 */
	bool		ms_initialized; /* are subplans started? */
	struct PartitionPruneState *ms_prune_state;
	Bitmapset  *ms_valid_subplans;
} MergeAppendState;

/* ----------------
 *	RecursiveUnionState 信息
 *
 *	RecursiveUnionState 用于执行递归并。
 *
 *	recursing 已扫完非递归项则为 T
 *	intermediate_empty intermediate_table 当前为空则为 T
 *	working_table 工作表(由递归项扫描)
 *	intermediate_table 当前递归输出(WT 的下一代)
 * ----------------
 */
typedef struct RecursiveUnionState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	bool		recursing;
	bool		intermediate_empty;
	Tuplestorestate *working_table;
	Tuplestorestate *intermediate_table;
	/* UNION ALL 时其余字段未用 */
	Oid		   *eqfuncoids;		/* 每分组字段的相等函数 */
	FmgrInfo   *hashfunctions;	/* 每分组字段哈希函数 */
	MemoryContext tempContext;	/* 比较用的短期上下文 */
	TupleHashTable hashtable;	/* 已见元组的哈希表 */
	MemoryContext tableContext; /* 含哈希表的内存上下文 */
} RecursiveUnionState;

/* ----------------
 *	BitmapAndState 信息
 * ----------------
 */
typedef struct BitmapAndState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	PlanState **bitmapplans;	/* 我的输入的 PlanStates 数组 */
	int			nplans;			/* 输入计划数 */
} BitmapAndState;

/* ----------------
 *	BitmapOrState 信息
 * ----------------
 */
typedef struct BitmapOrState
{
	PlanState	ps;				/* 其第一字段是 NodeTag */
	PlanState **bitmapplans;	/* 我的输入的 PlanStates 数组 */
	int			nplans;			/* 输入计划数 */
} BitmapOrState;

/* ----------------------------------------------------------------
 *	扫描状态信息
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 ScanState information
 *
 *		ScanState extends PlanState for node types that represent
 *		scans of an underlying relation.  It can also be used for nodes
 *		that scan the output of an underlying plan node --- in that case,
 *		only ScanTupleSlot is actually useful, and it refers to the tuple
 *		retrieved from the subplan.
 *
 *		currentRelation    relation being scanned (NULL if none)
 *		currentScanDesc    current scan descriptor for scan (NULL if none)
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 * ----------------
 */
typedef struct ScanState
{
	PlanState	ps;				/* its first field is NodeTag */
	Relation	ss_currentRelation;
	struct TableScanDescData *ss_currentScanDesc;
	TupleTableSlot *ss_ScanTupleSlot;
} ScanState;

/* ----------------
 *	 SeqScanState information
 * ----------------
 */
typedef struct SeqScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	Size		pscan_len;		/* size of parallel heap scan descriptor */
} SeqScanState;

/* ----------------
 *	 SampleScanState information
 * ----------------
 */
typedef struct SampleScanState
{
	ScanState	ss;
	List	   *args;			/* expr states for TABLESAMPLE params */
	ExprState  *repeatable;		/* expr state for REPEATABLE expr */
	/* use struct pointer to avoid including tsmapi.h here */
	struct TsmRoutine *tsmroutine;	/* descriptor for tablesample method */
	void	   *tsm_state;		/* tablesample method can keep state here */
	bool		use_bulkread;	/* use bulkread buffer access strategy? */
	bool		use_pagemode;	/* use page-at-a-time visibility checking? */
	bool		begun;			/* false means need to call BeginSampleScan */
	uint32		seed;			/* random seed */
	int64		donetuples;		/* number of tuples already returned */
	bool		haveblock;		/* has a block for sampling been determined */
	bool		done;			/* exhausted all tuples? */
} SampleScanState;

/*
 * These structs store information about index quals that don't have simple
 * constant right-hand sides.  See comments for ExecIndexBuildScanKeys()
 * for discussion.
 */
typedef struct
{
	struct ScanKeyData *scan_key;	/* scankey to put value into */
	ExprState  *key_expr;		/* expr to evaluate to get value */
	bool		key_toastable;	/* is expr's result a toastable datatype? */
} IndexRuntimeKeyInfo;

typedef struct
{
	struct ScanKeyData *scan_key;	/* scankey to put value into */
	ExprState  *array_expr;		/* expr to evaluate to get array value */
	int			next_elem;		/* next array element to use */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
	bool	   *elem_nulls;		/* array of num_elems is-null flags */
} IndexArrayKeyInfo;

/* ----------------
 *	 IndexScanState information
 *
 *		indexqualorig	   execution state for indexqualorig expressions
 *		indexorderbyorig   execution state for indexorderbyorig expressions
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		OrderByKeys		   Skey structures for index ordering operators
 *		NumOrderByKeys	   number of OrderByKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 *
 *		ReorderQueue	   tuples that need reordering due to re-check
 *		ReachedEnd		   have we fetched all tuples from index already?
 *		OrderByValues	   values of ORDER BY exprs of last fetched tuple
 *		OrderByNulls	   null flags for OrderByValues
 *		SortSupport		   for reordering ORDER BY exprs
 *		OrderByTypByVals   is the datatype of order by expression pass-by-value?
 *		OrderByTypLens	   typlens of the datatypes of order by expressions
 *		PscanLen		   size of parallel index scan descriptor
 * ----------------
 */
typedef struct IndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *indexqualorig;
	List	   *indexorderbyorig;
	struct ScanKeyData *iss_ScanKeys;
	int			iss_NumScanKeys;
	struct ScanKeyData *iss_OrderByKeys;
	int			iss_NumOrderByKeys;
	IndexRuntimeKeyInfo *iss_RuntimeKeys;
	int			iss_NumRuntimeKeys;
	bool		iss_RuntimeKeysReady;
	ExprContext *iss_RuntimeContext;
	Relation	iss_RelationDesc;
	struct IndexScanDescData *iss_ScanDesc;

	/* These are needed for re-checking ORDER BY expr ordering */
	pairingheap *iss_ReorderQueue;
	bool		iss_ReachedEnd;
	Datum	   *iss_OrderByValues;
	bool	   *iss_OrderByNulls;
	SortSupport iss_SortSupport;
	bool	   *iss_OrderByTypByVals;
	int16	   *iss_OrderByTypLens;
	Size		iss_PscanLen;
} IndexScanState;

/* ----------------
 *	 IndexOnlyScanState information
 *
 *		recheckqual		   execution state for recheckqual expressions
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		OrderByKeys		   Skey structures for index ordering operators
 *		NumOrderByKeys	   number of OrderByKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 *		TableSlot		   slot for holding tuples fetched from the table
 *		VMBuffer		   buffer in use for visibility map testing, if any
 *		PscanLen		   size of parallel index-only scan descriptor
 *		NameCStringAttNums attnums of name typed columns to pad to NAMEDATALEN
 *		NameCStringCount   number of elements in the NameCStringAttNums array
 * ----------------
 */
typedef struct IndexOnlyScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *recheckqual;
	struct ScanKeyData *ioss_ScanKeys;
	int			ioss_NumScanKeys;
	struct ScanKeyData *ioss_OrderByKeys;
	int			ioss_NumOrderByKeys;
	IndexRuntimeKeyInfo *ioss_RuntimeKeys;
	int			ioss_NumRuntimeKeys;
	bool		ioss_RuntimeKeysReady;
	ExprContext *ioss_RuntimeContext;
	Relation	ioss_RelationDesc;
	struct IndexScanDescData *ioss_ScanDesc;
	TupleTableSlot *ioss_TableSlot;
	Buffer		ioss_VMBuffer;
	Size		ioss_PscanLen;
	AttrNumber *ioss_NameCStringAttNums;
	int			ioss_NameCStringCount;
} IndexOnlyScanState;

/* ----------------
 *	 BitmapIndexScanState information
 *
 *		result			   bitmap to return output into, or NULL
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		ArrayKeys		   info about Skeys that come from ScalarArrayOpExprs
 *		NumArrayKeys	   number of ArrayKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 * ----------------
 */
typedef struct BitmapIndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	TIDBitmap  *biss_result;
	struct ScanKeyData *biss_ScanKeys;
	int			biss_NumScanKeys;
	IndexRuntimeKeyInfo *biss_RuntimeKeys;
	int			biss_NumRuntimeKeys;
	IndexArrayKeyInfo *biss_ArrayKeys;
	int			biss_NumArrayKeys;
	bool		biss_RuntimeKeysReady;
	ExprContext *biss_RuntimeContext;
	Relation	biss_RelationDesc;
	struct IndexScanDescData *biss_ScanDesc;
} BitmapIndexScanState;

/* ----------------
 *	 SharedBitmapState information
 *
 *		BM_INITIAL		TIDBitmap creation is not yet started, so first worker
 *						to see this state will set the state to BM_INPROGRESS
 *						and that process will be responsible for creating
 *						TIDBitmap.
 *		BM_INPROGRESS	TIDBitmap creation is in progress; workers need to
 *						sleep until it's finished.
 *		BM_FINISHED		TIDBitmap creation is done, so now all workers can
 *						proceed to iterate over TIDBitmap.
 * ----------------
 */
typedef enum
{
	BM_INITIAL,
	BM_INPROGRESS,
	BM_FINISHED
} SharedBitmapState;

/* ----------------
 *	 ParallelBitmapHeapState information
 *		tbmiterator				iterator for scanning current pages
 *		prefetch_iterator		iterator for prefetching ahead of current page
 *		mutex					mutual exclusion for the prefetching variable
 *								and state
 *		prefetch_pages			# pages prefetch iterator is ahead of current
 *		prefetch_target			current target prefetch distance
 *		state					current state of the TIDBitmap
 *		cv						conditional wait variable
 *		phs_snapshot_data		snapshot data shared to workers
 * ----------------
 */
typedef struct ParallelBitmapHeapState
{
	dsa_pointer tbmiterator;
	dsa_pointer prefetch_iterator;
	slock_t		mutex;
	int			prefetch_pages;
	int			prefetch_target;
	SharedBitmapState state;
	ConditionVariable cv;
	char		phs_snapshot_data[FLEXIBLE_ARRAY_MEMBER];
} ParallelBitmapHeapState;

/* ----------------
 *	 BitmapHeapScanState information
 *
 *		bitmapqualorig	   execution state for bitmapqualorig expressions
 *		tbm				   bitmap obtained from child index scan(s)
 *		tbmiterator		   iterator for scanning current pages
 *		tbmres			   current-page data
 *		can_skip_fetch	   can we potentially skip tuple fetches in this scan?
 *		return_empty_tuples number of empty tuples to return
 *		vmbuffer		   buffer for visibility-map lookups
 *		pvmbuffer		   ditto, for prefetched pages
 *		exact_pages		   total number of exact pages retrieved
 *		lossy_pages		   total number of lossy pages retrieved
 *		prefetch_iterator  iterator for prefetching ahead of current page
 *		prefetch_pages	   # pages prefetch iterator is ahead of current
 *		prefetch_target    current target prefetch distance
 *		prefetch_maximum   maximum value for prefetch_target
 *		pscan_len		   size of the shared memory for parallel bitmap
 *		initialized		   is node is ready to iterate
 *		shared_tbmiterator	   shared iterator
 *		shared_prefetch_iterator shared iterator for prefetching
 *		pstate			   shared state for parallel bitmap scan
 * ----------------
 */
typedef struct BitmapHeapScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *bitmapqualorig;
	TIDBitmap  *tbm;
	TBMIterator *tbmiterator;
	TBMIterateResult *tbmres;
	bool		can_skip_fetch;
	int			return_empty_tuples;
	Buffer		vmbuffer;
	Buffer		pvmbuffer;
	long		exact_pages;
	long		lossy_pages;
	TBMIterator *prefetch_iterator;
	int			prefetch_pages;
	int			prefetch_target;
	int			prefetch_maximum;
	Size		pscan_len;
	bool		initialized;
	TBMSharedIterator *shared_tbmiterator;
	TBMSharedIterator *shared_prefetch_iterator;
	ParallelBitmapHeapState *pstate;
} BitmapHeapScanState;

/* ----------------
 *	 TidScanState information
 *
 *		tidexprs	   list of TidExpr structs (see nodeTidscan.c)
 *		isCurrentOf    scan has a CurrentOfExpr qual
 *		NumTids		   number of tids in this scan
 *		TidPtr		   index of currently fetched tid
 *		TidList		   evaluated item pointers (array of size NumTids)
 *		htup		   currently-fetched tuple, if any
 * ----------------
 */
typedef struct TidScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *tss_tidexprs;
	bool		tss_isCurrentOf;
	int			tss_NumTids;
	int			tss_TidPtr;
	ItemPointerData *tss_TidList;
	HeapTupleData tss_htup;
} TidScanState;

/* ----------------
 *	 TidRangeScanState information
 *
 *		trss_tidexprs		list of TidOpExpr structs (see nodeTidrangescan.c)
 *		trss_mintid			the lowest TID in the scan range
 *		trss_maxtid			the highest TID in the scan range
 *		trss_inScan			is a scan currently in progress?
 * ----------------
 */
typedef struct TidRangeScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *trss_tidexprs;
	ItemPointerData trss_mintid;
	ItemPointerData trss_maxtid;
	bool		trss_inScan;
} TidRangeScanState;

/* ----------------
 *	 SubqueryScanState information
 *
 *		SubqueryScanState is used for scanning a sub-query in the range table.
 *		ScanTupleSlot references the current output tuple of the sub-query.
 * ----------------
 */
typedef struct SubqueryScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	PlanState  *subplan;
} SubqueryScanState;

/* ----------------
 *	 FunctionScanState information
 *
 *		Function nodes are used to scan the results of a
 *		function appearing in FROM (typically a function returning set).
 *
 *		eflags				node's capability flags
 *		ordinality			is this scan WITH ORDINALITY?
 *		simple				true if we have 1 function and no ordinality
 *		ordinal				current ordinal column value
 *		nfuncs				number of functions being executed
 *		funcstates			per-function execution states (private in
 *							nodeFunctionscan.c)
 *		argcontext			memory context to evaluate function arguments in
 * ----------------
 */
struct FunctionScanPerFuncState;

typedef struct FunctionScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;
	bool		ordinality;
	bool		simple;
	int64		ordinal;
	int			nfuncs;
	struct FunctionScanPerFuncState *funcstates;	/* array of length nfuncs */
	MemoryContext argcontext;
} FunctionScanState;

/* ----------------
 *	 ValuesScanState information
 *
 *		ValuesScan nodes are used to scan the results of a VALUES list
 *
 *		rowcontext			per-expression-list context
 *		exprlists			array of expression lists being evaluated
 *		exprstatelists		array of expression state lists, for SubPlans only
 *		array_len			size of above arrays
 *		curr_idx			current array index (0-based)
 *
 *	Note: ss.ps.ps_ExprContext is used to evaluate any qual or projection
 *	expressions attached to the node.  We create a second ExprContext,
 *	rowcontext, in which to build the executor expression state for each
 *	Values sublist.  Resetting this context lets us get rid of expression
 *	state for each row, avoiding major memory leakage over a long values list.
 *	However, that doesn't work for sublists containing SubPlans, because a
 *	SubPlan has to be connected up to the outer plan tree to work properly.
 *	Therefore, for only those sublists containing SubPlans, we do expression
 *	state construction at executor start, and store those pointers in
 *	exprstatelists[].  NULL entries in that array correspond to simple
 *	subexpressions that are handled as described above.
 * ----------------
 */
typedef struct ValuesScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprContext *rowcontext;
	List	  **exprlists;
	List	  **exprstatelists;
	int			array_len;
	int			curr_idx;
} ValuesScanState;

/* ----------------
 *		TableFuncScanState node
 *
 * Used in table-expression functions like XMLTABLE.
 * ----------------
 */
typedef struct TableFuncScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *docexpr;		/* state for document expression */
	ExprState  *rowexpr;		/* state for row-generating expression */
	List	   *colexprs;		/* state for column-generating expression */
	List	   *coldefexprs;	/* state for column default expressions */
	List	   *ns_names;		/* same as TableFunc.ns_names */
	List	   *ns_uris;		/* list of states of namespace URI exprs */
	Bitmapset  *notnulls;		/* nullability flag for each output column */
	void	   *opaque;			/* table builder private space */
	const struct TableFuncRoutine *routine; /* table builder methods */
	FmgrInfo   *in_functions;	/* input function for each column */
	Oid		   *typioparams;	/* typioparam for each column */
	int64		ordinal;		/* row number to be output next */
	MemoryContext perTableCxt;	/* per-table context */
	Tuplestorestate *tupstore;	/* output tuple store */
} TableFuncScanState;

/* ----------------
 *	 CteScanState information
 *
 *		CteScan nodes are used to scan a CommonTableExpr query.
 *
 * Multiple CteScan nodes can read out from the same CTE query.  We use
 * a tuplestore to hold rows that have been read from the CTE query but
 * not yet consumed by all readers.
 * ----------------
 */
typedef struct CteScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;			/* capability flags to pass to tuplestore */
	int			readptr;		/* index of my tuplestore read pointer */
	PlanState  *cteplanstate;	/* PlanState for the CTE query itself */
	/* Link to the "leader" CteScanState (possibly this same node) */
	struct CteScanState *leader;
	/* The remaining fields are only valid in the "leader" CteScanState */
	Tuplestorestate *cte_table; /* rows already read from the CTE query */
	bool		eof_cte;		/* reached end of CTE query? */
} CteScanState;

/* ----------------
 *	 NamedTuplestoreScanState information
 *
 *		NamedTuplestoreScan nodes are used to scan a Tuplestore created and
 *		named prior to execution of the query.  An example is a transition
 *		table for an AFTER trigger.
 *
 * Multiple NamedTuplestoreScan nodes can read out from the same Tuplestore.
 * ----------------
 */
typedef struct NamedTuplestoreScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			readptr;		/* index of my tuplestore read pointer */
	TupleDesc	tupdesc;		/* format of the tuples in the tuplestore */
	Tuplestorestate *relation;	/* the rows */
} NamedTuplestoreScanState;

/* ----------------
 *	 WorkTableScanState information
 *
 *		WorkTableScan nodes are used to scan the work table created by
 *		a RecursiveUnion node.  We locate the RecursiveUnion node
 *		during executor startup.
 * ----------------
 */
typedef struct WorkTableScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	RecursiveUnionState *rustate;
} WorkTableScanState;

/* ----------------
 *	 ForeignScanState information
 *
 *		ForeignScan nodes are used to scan foreign-data tables.
 * ----------------
 */
typedef struct ForeignScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *fdw_recheck_quals;	/* original quals not in ss.ps.qual */
	Size		pscan_len;		/* size of parallel coordination information */
	ResultRelInfo *resultRelInfo;	/* result rel info, if UPDATE or DELETE */
	/* use struct pointer to avoid including fdwapi.h here */
	struct FdwRoutine *fdwroutine;
	void	   *fdw_state;		/* foreign-data wrapper can keep state here */
} ForeignScanState;

/* ----------------
 *	 CustomScanState information
 *
 *		CustomScan nodes are used to execute custom code within executor.
 *
 * Core code must avoid assuming that the CustomScanState is only as large as
 * the structure declared here; providers are allowed to make it the first
 * element in a larger structure, and typically would need to do so.  The
 * struct is actually allocated by the CreateCustomScanState method associated
 * with the plan node.  Any additional fields can be initialized there, or in
 * the BeginCustomScan method.
 * ----------------
 */
struct CustomExecMethods;

typedef struct CustomScanState
{
	ScanState	ss;
	uint32		flags;			/* mask of CUSTOMPATH_* flags, see
								 * nodes/extensible.h */
	List	   *custom_ps;		/* list of child PlanState nodes, if any */
	Size		pscan_len;		/* size of parallel coordination information */
	const struct CustomExecMethods *methods;
	const struct TupleTableSlotOps *slotOps;
} CustomScanState;

/* ----------------------------------------------------------------
 *				 Join State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 JoinState information
 *
 *		Superclass for state nodes of join plans.
 * ----------------
 */
typedef struct JoinState
{
	PlanState	ps;
	JoinType	jointype;
	bool		single_match;	/* True if we should skip to next outer tuple
								 * after finding one inner match */
	ExprState  *joinqual;		/* JOIN quals (in addition to ps.qual) */
} JoinState;

/* ----------------
 *	 NestLoopState information
 *
 *		NeedNewOuter	   true if need new outer tuple on next call
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 * ----------------
 */
typedef struct NestLoopState
{
	JoinState	js;				/* its first field is NodeTag */
	bool		nl_NeedNewOuter;
	bool		nl_MatchedOuter;
	TupleTableSlot *nl_NullInnerTupleSlot;
} NestLoopState;

/* ----------------
 *	 MergeJoinState information
 *
 *		NumClauses		   number of mergejoinable join clauses
 *		Clauses			   info for each mergejoinable clause
 *		JoinState		   current state of ExecMergeJoin state machine
 *		SkipMarkRestore    true if we may skip Mark and Restore operations
 *		ExtraMarks		   true to issue extra Mark operations on inner scan
 *		ConstFalseJoin	   true if we have a constant-false joinqual
 *		FillOuter		   true if should emit unjoined outer tuples anyway
 *		FillInner		   true if should emit unjoined inner tuples anyway
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		MatchedInner	   true if found a join match for current inner tuple
 *		OuterTupleSlot	   slot in tuple table for cur outer tuple
 *		InnerTupleSlot	   slot in tuple table for cur inner tuple
 *		MarkedTupleSlot    slot in tuple table for marked tuple
 *		NullOuterTupleSlot prepared null tuple for right outer joins
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 *		OuterEContext	   workspace for computing outer tuple's join values
 *		InnerEContext	   workspace for computing inner tuple's join values
 * ----------------
 */
/* private in nodeMergejoin.c: */
typedef struct MergeJoinClauseData *MergeJoinClause;

typedef struct MergeJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	int			mj_NumClauses;
	MergeJoinClause mj_Clauses; /* array of length mj_NumClauses */
	int			mj_JoinState;
	bool		mj_SkipMarkRestore;
	bool		mj_ExtraMarks;
	bool		mj_ConstFalseJoin;
	bool		mj_FillOuter;
	bool		mj_FillInner;
	bool		mj_MatchedOuter;
	bool		mj_MatchedInner;
	TupleTableSlot *mj_OuterTupleSlot;
	TupleTableSlot *mj_InnerTupleSlot;
	TupleTableSlot *mj_MarkedTupleSlot;
	TupleTableSlot *mj_NullOuterTupleSlot;
	TupleTableSlot *mj_NullInnerTupleSlot;
	ExprContext *mj_OuterEContext;
	ExprContext *mj_InnerEContext;
} MergeJoinState;

/* ----------------
 *	 HashJoinState information
 *
 *		hashclauses				original form of the hashjoin condition
 *		hj_OuterHashKeys		the outer hash keys in the hashjoin condition
 *		hj_HashOperators		the join operators in the hashjoin condition
 *		hj_HashTable			hash table for the hashjoin
 *								(NULL if table not built yet)
 *		hj_CurHashValue			hash value for current outer tuple
 *		hj_CurBucketNo			regular bucket# for current outer tuple
 *		hj_CurSkewBucketNo		skew bucket# for current outer tuple
 *		hj_CurTuple				last inner tuple matched to current outer
 *								tuple, or NULL if starting search
 *								(hj_CurXXX variables are undefined if
 *								OuterTupleSlot is empty!)
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for inner (hashed) tuples
 *		hj_NullOuterTupleSlot	prepared null tuple for right/right-anti/full
 *								outer joins
 *		hj_NullInnerTupleSlot	prepared null tuple for left/full outer joins
 *		hj_FirstOuterTupleSlot	first tuple retrieved from outer plan
 *		hj_JoinState			current state of ExecHashJoin state machine
 *		hj_MatchedOuter			true if found a join match for current outer
 *		hj_OuterNotEmpty		true if outer relation known not empty
 * ----------------
 */

/* these structs are defined in executor/hashjoin.h: */
typedef struct HashJoinTupleData *HashJoinTuple;
typedef struct HashJoinTableData *HashJoinTable;

typedef struct HashJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	ExprState  *hashclauses;
	List	   *hj_OuterHashKeys;	/* list of ExprState nodes */
	List	   *hj_HashOperators;	/* list of operator OIDs */
	List	   *hj_Collations;
	HashJoinTable hj_HashTable;
	uint32		hj_CurHashValue;
	int			hj_CurBucketNo;
	int			hj_CurSkewBucketNo;
	HashJoinTuple hj_CurTuple;
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
	TupleTableSlot *hj_NullOuterTupleSlot;
	TupleTableSlot *hj_NullInnerTupleSlot;
	TupleTableSlot *hj_FirstOuterTupleSlot;
	int			hj_JoinState;
	bool		hj_MatchedOuter;
	bool		hj_OuterNotEmpty;
} HashJoinState;


/* ----------------------------------------------------------------
 *				 Materialization State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 MaterialState information
 *
 *		materialize nodes are used to materialize the results
 *		of a subplan into a temporary file.
 *
 *		ss.ss_ScanTupleSlot refers to output of underlying plan.
 * ----------------
 */
typedef struct MaterialState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;			/* capability flags to pass to tuplestore */
	bool		eof_underlying; /* reached end of underlying plan? */
	Tuplestorestate *tuplestorestate;
} MaterialState;

struct MemoizeEntry;
struct MemoizeTuple;
struct MemoizeKey;

typedef struct MemoizeInstrumentation
{
	uint64		cache_hits;		/* number of rescans where we've found the
								 * scan parameter values to be cached */
	uint64		cache_misses;	/* number of rescans where we've not found the
								 * scan parameter values to be cached. */
	uint64		cache_evictions;	/* number of cache entries removed due to
									 * the need to free memory */
	uint64		cache_overflows;	/* number of times we've had to bypass the
									 * cache when filling it due to not being
									 * able to free enough space to store the
									 * current scan's tuples. */
	uint64		mem_peak;		/* peak memory usage in bytes */
} MemoizeInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker memoize information
 * ----------------
 */
typedef struct SharedMemoizeInfo
{
	int			num_workers;
	MemoizeInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedMemoizeInfo;

/* ----------------
 *	 MemoizeState information
 *
 *		memoize nodes are used to cache recent and commonly seen results from
 *		a parameterized scan.
 * ----------------
 */
typedef struct MemoizeState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			mstatus;		/* value of ExecMemoize state machine */
	int			nkeys;			/* number of cache keys */
	struct memoize_hash *hashtable; /* hash table for cache entries */
	TupleDesc	hashkeydesc;	/* tuple descriptor for cache keys */
	TupleTableSlot *tableslot;	/* min tuple slot for existing cache entries */
	TupleTableSlot *probeslot;	/* virtual slot used for hash lookups */
	ExprState  *cache_eq_expr;	/* Compare exec params to hash key */
	ExprState **param_exprs;	/* exprs containing the parameters to this
								 * node */
	FmgrInfo   *hashfunctions;	/* lookup data for hash funcs nkeys in size */
	Oid		   *collations;		/* collation for comparisons nkeys in size */
	uint64		mem_used;		/* bytes of memory used by cache */
	uint64		mem_limit;		/* memory limit in bytes for the cache */
	MemoryContext tableContext; /* memory context to store cache data */
	dlist_head	lru_list;		/* least recently used entry list */
	struct MemoizeTuple *last_tuple;	/* Used to point to the last tuple
										 * returned during a cache hit and the
										 * tuple we last stored when
										 * populating the cache. */
	struct MemoizeEntry *entry; /* the entry that 'last_tuple' belongs to or
								 * NULL if 'last_tuple' is NULL. */
	bool		singlerow;		/* true if the cache entry is to be marked as
								 * complete after caching the first tuple. */
	bool		binary_mode;	/* true when cache key should be compared bit
								 * by bit, false when using hash equality ops */
	MemoizeInstrumentation stats;	/* execution statistics */
	SharedMemoizeInfo *shared_info; /* statistics for parallel workers */
	Bitmapset  *keyparamids;	/* Param->paramids of expressions belonging to
								 * param_exprs */
} MemoizeState;

/* ----------------
 *	 When performing sorting by multiple keys, it's possible that the input
 *	 dataset is already sorted on a prefix of those keys. We call these
 *	 "presorted keys".
 *	 PresortedKeyData represents information about one such key.
 * ----------------
 */
typedef struct PresortedKeyData
{
	FmgrInfo	flinfo;			/* comparison function info */
	FunctionCallInfo fcinfo;	/* comparison function call info */
	OffsetNumber attno;			/* attribute number in tuple */
} PresortedKeyData;

/* ----------------
 *	 Shared memory container for per-worker sort information
 * ----------------
 */
typedef struct SharedSortInfo
{
	int			num_workers;
	TuplesortInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedSortInfo;

/* ----------------
 *	 SortState information
 * ----------------
 */
typedef struct SortState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		randomAccess;	/* need random access to sort output? */
	bool		bounded;		/* is the result set bounded? */
	int64		bound;			/* if bounded, how many tuples are needed */
	bool		sort_Done;		/* sort completed yet? */
	bool		bounded_Done;	/* value of bounded we did the sort with */
	int64		bound_Done;		/* value of bound we did the sort with */
	void	   *tuplesortstate; /* private state of tuplesort.c */
	bool		am_worker;		/* are we a worker? */
	bool		datumSort;		/* Datum sort instead of tuple sort? */
	SharedSortInfo *shared_info;	/* one entry per worker */
} SortState;

/* ----------------
 *	 Instrumentation information for IncrementalSort
 * ----------------
 */
typedef struct IncrementalSortGroupInfo
{
	int64		groupCount;
	int64		maxDiskSpaceUsed;
	int64		totalDiskSpaceUsed;
	int64		maxMemorySpaceUsed;
	int64		totalMemorySpaceUsed;
	bits32		sortMethods;	/* bitmask of TuplesortMethod */
} IncrementalSortGroupInfo;

typedef struct IncrementalSortInfo
{
	IncrementalSortGroupInfo fullsortGroupInfo;
	IncrementalSortGroupInfo prefixsortGroupInfo;
} IncrementalSortInfo;

/* ----------------
 *	 Shared memory container for per-worker incremental sort information
 * ----------------
 */
typedef struct SharedIncrementalSortInfo
{
	int			num_workers;
	IncrementalSortInfo sinfo[FLEXIBLE_ARRAY_MEMBER];
} SharedIncrementalSortInfo;

/* ----------------
 *	 IncrementalSortState information
 * ----------------
 */
typedef enum
{
	INCSORT_LOADFULLSORT,
	INCSORT_LOADPREFIXSORT,
	INCSORT_READFULLSORT,
	INCSORT_READPREFIXSORT,
} IncrementalSortExecutionStatus;

typedef struct IncrementalSortState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		bounded;		/* is the result set bounded? */
	int64		bound;			/* if bounded, how many tuples are needed */
	bool		outerNodeDone;	/* finished fetching tuples from outer node */
	int64		bound_Done;		/* value of bound we did the sort with */
	IncrementalSortExecutionStatus execution_status;
	int64		n_fullsort_remaining;
	Tuplesortstate *fullsort_state; /* private state of tuplesort.c */
	Tuplesortstate *prefixsort_state;	/* private state of tuplesort.c */
	/* the keys by which the input path is already sorted */
	PresortedKeyData *presorted_keys;

	IncrementalSortInfo incsort_info;

	/* slot for pivot tuple defining values of presorted keys within group */
	TupleTableSlot *group_pivot;
	TupleTableSlot *transfer_tuple;
	bool		am_worker;		/* are we a worker? */
	SharedIncrementalSortInfo *shared_info; /* one entry per worker */
} IncrementalSortState;

/* ---------------------
 *	GroupState information
 * ---------------------
 */
typedef struct GroupState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *eqfunction;		/* equality function */
	bool		grp_done;		/* indicates completion of Group scan */
} GroupState;

/* ---------------------
 *	per-worker aggregate information
 * ---------------------
 */
typedef struct AggregateInstrumentation
{
	Size		hash_mem_peak;	/* peak hash table memory usage */
	uint64		hash_disk_used; /* kB of disk space used */
	int			hash_batches_used;	/* batches used during entire execution */
} AggregateInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker aggregate information
 * ----------------
 */
typedef struct SharedAggInfo
{
	int			num_workers;
	AggregateInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedAggInfo;

/* ---------------------
 *	AggState information
 *
 *	ss.ss_ScanTupleSlot refers to output of underlying plan.
 *
 *	Note: ss.ps.ps_ExprContext contains ecxt_aggvalues and
 *	ecxt_aggnulls arrays, which hold the computed agg values for the current
 *	input group during evaluation of an Agg node's output tuple(s).  We
 *	create a second ExprContext, tmpcontext, in which to evaluate input
 *	expressions and run the aggregate transition functions.
 * ---------------------
 */
/* these structs are private in nodeAgg.c: */
typedef struct AggStatePerAggData *AggStatePerAgg;
typedef struct AggStatePerTransData *AggStatePerTrans;
typedef struct AggStatePerGroupData *AggStatePerGroup;
typedef struct AggStatePerPhaseData *AggStatePerPhase;
typedef struct AggStatePerHashData *AggStatePerHash;

typedef struct AggState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	int			numtrans;		/* number of pertrans items */
	AggStrategy aggstrategy;	/* strategy mode */
	AggSplit	aggsplit;		/* agg-splitting mode, see nodes.h */
	AggStatePerPhase phase;		/* pointer to current phase data */
	int			numphases;		/* number of phases (including phase 0) */
	int			current_phase;	/* current phase number */
	AggStatePerAgg peragg;		/* per-Aggref information */
	AggStatePerTrans pertrans;	/* per-Trans state information */
	ExprContext *hashcontext;	/* econtexts for long-lived data (hashtable) */
	ExprContext **aggcontexts;	/* econtexts for long-lived data (per GS) */
	ExprContext *tmpcontext;	/* econtext for input expressions */
#define FIELDNO_AGGSTATE_CURAGGCONTEXT 14
	ExprContext *curaggcontext; /* currently active aggcontext */
	AggStatePerAgg curperagg;	/* currently active aggregate, if any */
#define FIELDNO_AGGSTATE_CURPERTRANS 16
	AggStatePerTrans curpertrans;	/* currently active trans state, if any */
	bool		input_done;		/* indicates end of input */
	bool		agg_done;		/* indicates completion of Agg scan */
	int			projected_set;	/* The last projected grouping set */
#define FIELDNO_AGGSTATE_CURRENT_SET 20
	int			current_set;	/* The current grouping set being evaluated */
	Bitmapset  *grouped_cols;	/* grouped cols in current projection */
	List	   *all_grouped_cols;	/* list of all grouped cols in DESC order */
	Bitmapset  *colnos_needed;	/* all columns needed from the outer plan */
	int			max_colno_needed;	/* highest colno needed from outer plan */
	bool		all_cols_needed;	/* are all cols from outer plan needed? */
	/* These fields are for grouping set phase data */
	int			maxsets;		/* The max number of sets in any phase */
	AggStatePerPhase phases;	/* array of all phases */
	Tuplesortstate *sort_in;	/* sorted input to phases > 1 */
	Tuplesortstate *sort_out;	/* input is copied here for next phase */
	TupleTableSlot *sort_slot;	/* slot for sort results */
	/* these fields are used in AGG_PLAIN and AGG_SORTED modes: */
	AggStatePerGroup *pergroups;	/* grouping set indexed array of per-group
									 * pointers */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in AGG_HASHED and AGG_MIXED modes: */
	bool		table_filled;	/* hash table filled yet? */
	int			num_hashes;
	MemoryContext hash_metacxt; /* memory for hash table itself */
	struct LogicalTapeSet *hash_tapeset;	/* tape set for hash spill tapes */
	struct HashAggSpill *hash_spills;	/* HashAggSpill for each grouping set,
										 * exists only during first pass */
	TupleTableSlot *hash_spill_rslot;	/* for reading spill files */
	TupleTableSlot *hash_spill_wslot;	/* for writing spill files */
	List	   *hash_batches;	/* hash batches remaining to be processed */
	bool		hash_ever_spilled;	/* ever spilled during this execution? */
	bool		hash_spill_mode;	/* we hit a limit during the current batch
									 * and we must not create new groups */
	Size		hash_mem_limit; /* limit before spilling hash table */
	uint64		hash_ngroups_limit; /* limit before spilling hash table */
	int			hash_planned_partitions;	/* number of partitions planned
											 * for first pass */
	double		hashentrysize;	/* estimate revised during execution */
	Size		hash_mem_peak;	/* peak hash table memory usage */
	uint64		hash_ngroups_current;	/* number of groups currently in
										 * memory in all hash tables */
	uint64		hash_disk_used; /* kB of disk space used */
	int			hash_batches_used;	/* batches used during entire execution */

	AggStatePerHash perhash;	/* array of per-hashtable data */
	AggStatePerGroup *hash_pergroup;	/* grouping set indexed array of
										 * per-group pointers */

	/* support for evaluation of agg input expressions: */
#define FIELDNO_AGGSTATE_ALL_PERGROUPS 53
	AggStatePerGroup *all_pergroups;	/* array of first ->pergroups, than
										 * ->hash_pergroup */
	ProjectionInfo *combinedproj;	/* projection machinery */
	SharedAggInfo *shared_info; /* one entry per worker */
} AggState;

/* ----------------
 *	WindowAggState information
 * ----------------
 */
/* these structs are private in nodeWindowAgg.c: */
typedef struct WindowStatePerFuncData *WindowStatePerFunc;
typedef struct WindowStatePerAggData *WindowStatePerAgg;

/*
 * WindowAggStatus -- Used to track the status of WindowAggState
 */
typedef enum WindowAggStatus
{
	WINDOWAGG_DONE,				/* No more processing to do */
	WINDOWAGG_RUN,				/* Normal processing of window funcs */
	WINDOWAGG_PASSTHROUGH,		/* Don't eval window funcs */
	WINDOWAGG_PASSTHROUGH_STRICT	/* Pass-through plus don't store new
									 * tuples during spool */
} WindowAggStatus;

typedef struct WindowAggState
{
	ScanState	ss;				/* its first field is NodeTag */

	/* these fields are filled in by ExecInitExpr: */
	List	   *funcs;			/* all WindowFunc nodes in targetlist */
	int			numfuncs;		/* total number of window functions */
	int			numaggs;		/* number that are plain aggregates */

	WindowStatePerFunc perfunc; /* per-window-function information */
	WindowStatePerAgg peragg;	/* per-plain-aggregate information */
	ExprState  *partEqfunction; /* equality funcs for partition columns */
	ExprState  *ordEqfunction;	/* equality funcs for ordering columns */
	Tuplestorestate *buffer;	/* stores rows of current partition */
	int			current_ptr;	/* read pointer # for current row */
	int			framehead_ptr;	/* read pointer # for frame head, if used */
	int			frametail_ptr;	/* read pointer # for frame tail, if used */
	int			grouptail_ptr;	/* read pointer # for group tail, if used */
	int64		spooled_rows;	/* total # of rows in buffer */
	int64		currentpos;		/* position of current row in partition */
	int64		frameheadpos;	/* current frame head position */
	int64		frametailpos;	/* current frame tail position (frame end+1) */
	/* use struct pointer to avoid including windowapi.h here */
	struct WindowObjectData *agg_winobj;	/* winobj for aggregate fetches */
	int64		aggregatedbase; /* start row for current aggregates */
	int64		aggregatedupto; /* rows before this one are aggregated */
	WindowAggStatus status;		/* run status of WindowAggState */

	int			frameOptions;	/* frame_clause options, see WindowDef */
	ExprState  *startOffset;	/* expression for starting bound offset */
	ExprState  *endOffset;		/* expression for ending bound offset */
	Datum		startOffsetValue;	/* result of startOffset evaluation */
	Datum		endOffsetValue; /* result of endOffset evaluation */

	/* these fields are used with RANGE offset PRECEDING/FOLLOWING: */
	FmgrInfo	startInRangeFunc;	/* in_range function for startOffset */
	FmgrInfo	endInRangeFunc; /* in_range function for endOffset */
	Oid			inRangeColl;	/* collation for in_range tests */
	bool		inRangeAsc;		/* use ASC sort order for in_range tests? */
	bool		inRangeNullsFirst;	/* nulls sort first for in_range tests? */

	/* these fields are used in GROUPS mode: */
	int64		currentgroup;	/* peer group # of current row in partition */
	int64		frameheadgroup; /* peer group # of frame head row */
	int64		frametailgroup; /* peer group # of frame tail row */
	int64		groupheadpos;	/* current row's peer group head position */
	int64		grouptailpos;	/* " " " " tail position (group end+1) */

	MemoryContext partcontext;	/* context for partition-lifespan data */
	MemoryContext aggcontext;	/* shared context for aggregate working data */
	MemoryContext curaggcontext;	/* current aggregate's working data */
	ExprContext *tmpcontext;	/* short-term evaluation context */

	ExprState  *runcondition;	/* Condition which must remain true otherwise
								 * execution of the WindowAgg will finish or
								 * go into pass-through mode.  NULL when there
								 * is no such condition. */

	bool		use_pass_through;	/* When false, stop execution when
									 * runcondition is no longer true.  Else
									 * just stop evaluating window funcs. */
	bool		top_window;		/* true if this is the top-most WindowAgg or
								 * the only WindowAgg in this query level */
	bool		all_first;		/* true if the scan is starting */
	bool		partition_spooled;	/* true if all tuples in current partition
									 * have been spooled into tuplestore */
	bool		more_partitions;	/* true if there's more partitions after
									 * this one */
	bool		framehead_valid;	/* true if frameheadpos is known up to
									 * date for current row */
	bool		frametail_valid;	/* true if frametailpos is known up to
									 * date for current row */
	bool		grouptail_valid;	/* true if grouptailpos is known up to
									 * date for current row */

	TupleTableSlot *first_part_slot;	/* first tuple of current or next
										 * partition */
	TupleTableSlot *framehead_slot; /* first tuple of current frame */
	TupleTableSlot *frametail_slot; /* first tuple after current frame */

	/* temporary slots for tuples fetched back from tuplestore */
	TupleTableSlot *agg_row_slot;
	TupleTableSlot *temp_slot_1;
	TupleTableSlot *temp_slot_2;
} WindowAggState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.  Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple (stored in its result slot).
 *		If the two are identical in all interesting fields, then
 *		we just fetch another tuple from the sort and try again.
 * ----------------
 */
typedef struct UniqueState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *eqfunction;		/* tuple equality qual */
} UniqueState;

/* ----------------
 * GatherState information
 *
 *		Gather nodes launch 1 or more parallel workers, run a subplan
 *		in those workers, and collect the results.
 * ----------------
 */
typedef struct GatherState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		initialized;	/* workers launched? */
	bool		need_to_scan_locally;	/* need to read from local plan? */
	int64		tuples_needed;	/* tuple bound, see ExecSetTupleBound */
	/* these fields are set up once: */
	TupleTableSlot *funnel_slot;
	struct ParallelExecutorInfo *pei;
	/* all remaining fields are reinitialized during a rescan: */
	int			nworkers_launched;	/* original number of workers */
	int			nreaders;		/* number of still-active workers */
	int			nextreader;		/* next one to try to read from */
	struct TupleQueueReader **reader;	/* array with nreaders active entries */
} GatherState;

/* ----------------
 * GatherMergeState information
 *
 *		Gather merge nodes launch 1 or more parallel workers, run a
 *		subplan which produces sorted output in each worker, and then
 *		merge the results into a single sorted stream.
 * ----------------
 */
struct GMReaderTupleBuffer;		/* private in nodeGatherMerge.c */

typedef struct GatherMergeState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		initialized;	/* workers launched? */
	bool		gm_initialized; /* gather_merge_init() done? */
	bool		need_to_scan_locally;	/* need to read from local plan? */
	int64		tuples_needed;	/* tuple bound, see ExecSetTupleBound */
	/* these fields are set up once: */
	TupleDesc	tupDesc;		/* descriptor for subplan result tuples */
	int			gm_nkeys;		/* number of sort columns */
	SortSupport gm_sortkeys;	/* array of length gm_nkeys */
	struct ParallelExecutorInfo *pei;
	/* all remaining fields are reinitialized during a rescan */
	/* (but the arrays are not reallocated, just cleared) */
	int			nworkers_launched;	/* original number of workers */
	int			nreaders;		/* number of active workers */
	TupleTableSlot **gm_slots;	/* array with nreaders+1 entries */
	struct TupleQueueReader **reader;	/* array with nreaders active entries */
	struct GMReaderTupleBuffer *gm_tuple_buffers;	/* nreaders tuple buffers */
	struct binaryheap *gm_heap; /* binary heap of slot indices */
} GatherMergeState;

/* ----------------
 *	 Values displayed by EXPLAIN ANALYZE
 * ----------------
 */
typedef struct HashInstrumentation
{
	int			nbuckets;		/* number of buckets at end of execution */
	int			nbuckets_original;	/* planned number of buckets */
	int			nbatch;			/* number of batches at end of execution */
	int			nbatch_original;	/* planned number of batches */
	Size		space_peak;		/* peak memory usage in bytes */
} HashInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker hash information
 * ----------------
 */
typedef struct SharedHashInfo
{
	int			num_workers;
	HashInstrumentation hinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedHashInfo;

/* ----------------
 *	 HashState information
 * ----------------
 */
typedef struct HashState
{
	PlanState	ps;				/* its first field is NodeTag */
	HashJoinTable hashtable;	/* hash table for the hashjoin */
	List	   *hashkeys;		/* list of ExprState nodes */

	/*
	 * In a parallelized hash join, the leader retains a pointer to the
	 * shared-memory stats area in its shared_info field, and then copies the
	 * shared-memory info back to local storage before DSM shutdown.  The
	 * shared_info field remains NULL in workers, or in non-parallel joins.
	 */
	SharedHashInfo *shared_info;

	/*
	 * If we are collecting hash stats, this points to an initially-zeroed
	 * collection area, which could be either local storage or in shared
	 * memory; either way it's for just one process.
	 */
	HashInstrumentation *hinstrument;

	/* Parallel hash state. */
	struct ParallelHashJoinState *parallel_state;
} HashState;

/* ----------------
 *	 SetOpState information
 *
 *		Even in "sorted" mode, SetOp nodes are more complex than a simple
 *		Unique, since we have to count how many duplicates to return.  But
 *		we also support hashing, so this is really more like a cut-down
 *		form of Agg.
 * ----------------
 */
/* this struct is private in nodeSetOp.c: */
typedef struct SetOpStatePerGroupData *SetOpStatePerGroup;

typedef struct SetOpState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *eqfunction;		/* equality comparator */
	Oid		   *eqfuncoids;		/* per-grouping-field equality fns */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	bool		setop_done;		/* indicates completion of output scan */
	long		numOutput;		/* number of dups left to output */
	/* these fields are used in SETOP_SORTED mode: */
	SetOpStatePerGroup pergroup;	/* per-group working state */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in SETOP_HASHED mode: */
	TupleHashTable hashtable;	/* hash table with one entry per group */
	MemoryContext tableContext; /* memory context containing hash table */
	bool		table_filled;	/* hash table filled yet? */
	TupleHashIterator hashiter; /* for iterating through hash table */
} SetOpState;

/* ----------------
 *	 LockRowsState information
 *
 *		LockRows nodes are used to enforce FOR [KEY] UPDATE/SHARE locking.
 * ----------------
 */
typedef struct LockRowsState
{
	PlanState	ps;				/* its first field is NodeTag */
	List	   *lr_arowMarks;	/* List of ExecAuxRowMarks */
	EPQState	lr_epqstate;	/* for evaluating EvalPlanQual rechecks */
} LockRowsState;

/* ----------------
 *	 LimitState information
 *
 *		Limit nodes are used to enforce LIMIT/OFFSET clauses.
 *		They just select the desired subrange of their subplan's output.
 *
 * offset is the number of initial tuples to skip (0 does nothing).
 * count is the number of tuples to return after skipping the offset tuples.
 * If no limit count was specified, count is undefined and noCount is true.
 * When lstate == LIMIT_INITIAL, offset/count/noCount haven't been set yet.
 * ----------------
 */
typedef enum
{
	LIMIT_INITIAL,				/* initial state for LIMIT node */
	LIMIT_RESCAN,				/* rescan after recomputing parameters */
	LIMIT_EMPTY,				/* there are no returnable rows */
	LIMIT_INWINDOW,				/* have returned a row in the window */
	LIMIT_WINDOWEND_TIES,		/* have returned a tied row */
	LIMIT_SUBPLANEOF,			/* at EOF of subplan (within window) */
	LIMIT_WINDOWEND,			/* stepped off end of window */
	LIMIT_WINDOWSTART			/* stepped off beginning of window */
} LimitStateCond;

typedef struct LimitState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *limitOffset;	/* OFFSET parameter, or NULL if none */
	ExprState  *limitCount;		/* COUNT parameter, or NULL if none */
	LimitOption limitOption;	/* limit specification type */
	int64		offset;			/* current OFFSET value */
	int64		count;			/* current COUNT, if any */
	bool		noCount;		/* if true, ignore count */
	LimitStateCond lstate;		/* state machine status, as above */
	int64		position;		/* 1-based index of last tuple returned */
	TupleTableSlot *subSlot;	/* tuple last obtained from subplan */
	ExprState  *eqfunction;		/* tuple equality qual in case of WITH TIES
								 * option */
	TupleTableSlot *last_slot;	/* slot for evaluation of ties */
} LimitState;

#endif							/* EXECNODES_H */
