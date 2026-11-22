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
 *	ScanState 信息
 *
 *	ScanState 扩展 PlanState,用于表示
 *	对底层关系做扫描的节点类型。也可用于
 *	扫描底层计划节点输出的节点 --- 此时仅
 *	ScanTupleSlot 实际有用,它指向自子计划
 *	取回的元组。
 *
 *	currentRelation 正在扫描的关系(无则为 NULL)
 *	currentScanDesc 当前扫描的扫描描述符(无则为 NULL)
 *	ScanTupleSlot 元组表中保存扫描元组的槽指针
 * ----------------
 */
typedef struct ScanState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	Relation	ss_currentRelation;
	struct TableScanDescData *ss_currentScanDesc;
	TupleTableSlot *ss_ScanTupleSlot;
} ScanState;

/* ----------------
 *	SeqScanState 信息
 * ----------------
 */
typedef struct SeqScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	Size		pscan_len;		/* 并行堆扫描描述符的大小 */
} SeqScanState;

/* ----------------
 *	SampleScanState 信息
 * ----------------
 */
typedef struct SampleScanState
{
	ScanState	ss;
	List	   *args;			/* TABLESAMPLE 参数表达式状态 */
	ExprState  *repeatable;		/* REPEATABLE 表达式的状态 */
	/* 用结构指针以免在此包含 tsmapi.h */
	struct TsmRoutine *tsmroutine;	/* tablesample 方法的描述符 */
	void	   *tsm_state;		/* tablesample 方法可存状态于此 */
	bool		use_bulkread;	/* 用 bulkread 缓冲访问策略? */
	bool		use_pagemode;	/* 是否逐页做可见性检查? */
	bool		begun;			/* false 表示需调用 BeginSampleScan */
	uint32		seed;			/* 随机种 */
	int64		donetuples;		/* 已返回的元组数 */
	bool		haveblock;		/* 是否已确定用于采样的块 */
	bool		done;			/* 耗尽全部元组? */
} SampleScanState;

/*
 * 这些结构保存关于无简单常量右值的
 * 索引限定信息。详见 ExecIndexBuildScanKeys() 的
 * 注释。
 */
typedef struct
{
	struct ScanKeyData *scan_key;	/* 要填入值的 scankey */
	ExprState  *key_expr;		/* 求值得值的表达式 */
	bool		key_toastable;	/* expr 结果是否可 TOAST? */
} IndexRuntimeKeyInfo;

typedef struct
{
	struct ScanKeyData *scan_key;	/* 要填入值的 scankey */
	ExprState  *array_expr;		/* 求值得数组值的表达式 */
	int			next_elem;		/* 下一要用数组元素 */
	int			num_elems;		/* 当前数组值中的元素数 */
	Datum	   *elem_values;	/* num_elems Datum 数组 */
	bool	   *elem_nulls;		/* num_elems 个空标志数组 */
} IndexArrayKeyInfo;

/* ----------------
 *	IndexScanState 信息
 *
 *	indexqualorig 表达式的执行状态
 *	indexorderbyorig 表达式的执行状态
 *	ScanKeys 索引限定的 Skey 结构
 *	NumScanKeys ScanKeys 数
 *	OrderByKeys 索引排序运算符的 Skey 结构
 *	NumOrderByKeys OrderByKeys 数
 *	RuntimeKeys 须在运行时求值的 Skey 信息
 *	NumRuntimeKeys RuntimeKeys 数
 *	RuntimeKeysReady 运行时 Skey 已算好则为真
 *	RuntimeContext 求值运行时 Skey 的表达式上下文
 *	RelationDesc 索引关系描述符
 *	ScanDesc 索引扫描描述符
 *
 *	ReorderQueue 因重检而需重排的元组
 *	ReachedEnd 是否已取尽索引中全部元组?
 *	OrderByValues 上次取到元组的 ORDER BY 表达式值
 *	OrderByNulls OrderByValues 的空标志
 *	SortSupport 供重排 ORDER BY 表达式
 *	OrderByTypByVals ORDER BY 表达式的数据类型是否传值?
 *	OrderByTypLens ORDER BY 表达式数据类型的 typlen
 *	PscanLen 并行索引扫描描述符的大小
 * ----------------
 */
typedef struct IndexScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
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

	/* 这些用于重检 ORDER BY 表达式排序 */
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
 *	IndexOnlyScanState 信息
 *
 *	recheckqual 表达式的执行状态
 *	ScanKeys 索引限定的 Skey 结构
 *	NumScanKeys ScanKeys 数
 *	OrderByKeys 索引排序运算符的 Skey 结构
 *	NumOrderByKeys OrderByKeys 数
 *	RuntimeKeys 须在运行时求值的 Skey 信息
 *	NumRuntimeKeys RuntimeKeys 数
 *	RuntimeKeysReady 运行时 Skey 已算好则为真
 *	RuntimeContext 求值运行时 Skey 的表达式上下文
 *	RelationDesc 索引关系描述符
 *	ScanDesc 索引扫描描述符
 *	TableSlot 保存从表取回的元组的槽
 *	VMBuffer 可见性映射测试所用缓冲(若有)
 *	PscanLen 并行仅索引扫描描述符的大小
 *	NameCStringAttNums 须填至 NAMEDATALEN 的 name 类型列 attnum
 *	NameCStringCount NameCStringAttNums 数组中的元素数
 * ----------------
 */
typedef struct IndexOnlyScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
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
 *	BitmapIndexScanState 信息
 *
 *	result 输出位图写入处,或为 NULL
 *	ScanKeys 索引限定的 Skey 结构
 *	NumScanKeys ScanKeys 数
 *	RuntimeKeys 须在运行时求值的 Skey 信息
 *	NumRuntimeKeys RuntimeKeys 数
 *	ArrayKeys 源自 ScalarArrayOpExprs 的 Skey 信息
 *	NumArrayKeys ArrayKeys 数
 *	RuntimeKeysReady 运行时 Skey 已算好则为真
 *	RuntimeContext 求值运行时 Skey 的表达式上下文
 *	RelationDesc 索引关系描述符
 *	ScanDesc 索引扫描描述符
 * ----------------
 */
typedef struct BitmapIndexScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
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
 *	SharedBitmapState 信息
 *
 *	BM_INITIAL TIDBitmap 创建尚未开始,故首个工作者
 *	见到本状态时会把状态设为 BM_INPROGRESS
 *	且该进程将负责创建
 *	TIDBitmap。
 *	BM_INPROGRESS TIDBitmap 正在创建;工作者须
 *	睡眠直至完成。
 *	BM_FINISHED TIDBitmap 已创建完,故全部工作者现可
 *	继续迭代 TIDBitmap。
 * ----------------
 */
typedef enum
{
	BM_INITIAL,
	BM_INPROGRESS,
	BM_FINISHED
} SharedBitmapState;

/* ----------------
 *	ParallelBitmapHeapState 信息
 *	tbmiterator 扫描当前页的迭代器
 *	prefetch_iterator 超前于当前页做预取的迭代器
 *	mutex 对预取变量
 *	与状态
 *	prefetch_pages 预取迭代器超前于当前的页数
 *	prefetch_target 当前目标预取距离
 *	state TIDBitmap 的当前状态
 *	cv 条件等待变量
 *	phs_snapshot_data 共享给工作者的快照数据
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
 *	BitmapHeapScanState 信息
 *
 *	bitmapqualorig 表达式的执行状态
 *	tbm 自子索引扫描得到的位图
 *	tbmiterator 扫描当前页的迭代器
 *	tbmres 当前页数据
 *	can_skip_fetch 本扫描是否可能跳过元组取回?
 *	return_empty_tuples 要返回的空元组数
 *	vmbuffer 可见性映射查找用缓冲
 *	pvmbuffer 同上,供预取页
 *	exact_pages 取回的精确页总数
 *	lossy_pages 取回的损耗页总数
 *	prefetch_iterator 超前于当前页做预取的迭代器
 *	prefetch_pages 预取迭代器超前于当前的页数
 *	prefetch_target 当前目标预取距离
 *	prefetch_maximum prefetch_target 的最大值
 *	pscan_len 并行位图共享内存的大小
 *	initialized 节点是否已就绪可迭代
 *	shared_tbmiterator 共享迭代器
 *	shared_prefetch_iterator 预取用共享迭代器
 *	pstate 并行位图扫描的共享状态
 * ----------------
 */
typedef struct BitmapHeapScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
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
 *	TidScanState 信息
 *
 *	tidexprs TidExpr 结构列表(见 nodeTidscan.c)
 *	isCurrentOf 扫描含 CurrentOfExpr 限定
 *	NumTids 本扫描中的 tid 数
 *	TidPtr 当前已取 tid 的下标
 *	TidList 已求值的项指针(大小为 NumTids 的数组)
 *	htup 当前已取元组(若有)
 * ----------------
 */
typedef struct TidScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	List	   *tss_tidexprs;
	bool		tss_isCurrentOf;
	int			tss_NumTids;
	int			tss_TidPtr;
	ItemPointerData *tss_TidList;
	HeapTupleData tss_htup;
} TidScanState;

/* ----------------
 *	TidRangeScanState 信息
 *
 *	trss_tidexprs TidOpExpr 结构列表(见 nodeTidrangescan.c)
 *	trss_mintid 扫描范围内最低 TID
 *	trss_maxtid 扫描范围内最高 TID
 *	trss_inScan 扫描是否正在进行?
 * ----------------
 */
typedef struct TidRangeScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	List	   *trss_tidexprs;
	ItemPointerData trss_mintid;
	ItemPointerData trss_maxtid;
	bool		trss_inScan;
} TidRangeScanState;

/* ----------------
 *	SubqueryScanState 信息
 *
 *	SubqueryScanState 用于扫描范围表中的子查询。
 *	ScanTupleSlot 引用子查询的当前输出元组。
 * ----------------
 */
typedef struct SubqueryScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	PlanState  *subplan;
} SubqueryScanState;

/* ----------------
 *	FunctionScanState 信息
 *
 *	Function 节点用于扫描出现在 FROM 中的
 *	函数结果(通常为返回集合的函数)。
 *
 *	eflags 节点的能力标志
 *	ordinality 本扫描是否 WITH ORDINALITY?
 *	simple 仅 1 个函数且无 ordinality 则为真
 *	ordinal 当前序数列的值
 *	nfuncs 正在执行的函数数
 *	funcstates 每函数的执行状态(私有于
 *	nodeFunctionscan.c)
 *	argcontext 求值函数参数所用的内存上下文
 * ----------------
 */
struct FunctionScanPerFuncState;

typedef struct FunctionScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	int			eflags;
	bool		ordinality;
	bool		simple;
	int64		ordinal;
	int			nfuncs;
	struct FunctionScanPerFuncState *funcstates;	/* nfuncs 长的数组 */
	MemoryContext argcontext;
} FunctionScanState;

/* ----------------
 *	ValuesScanState 信息
 *
 *	ValuesScan 节点用于扫描 VALUES 列表的结果
 *
 *	rowcontext 每表达式列表的上下文
 *	exprlists 正在求值的表达式列表数组
 *	exprstatelists 表达式状态列表数组,仅供 SubPlan
 *	array_len 上述数组的大小
 *	curr_idx 当前数组下标(从 0 起)
 *
 *	注: ss.ps.ps_ExprContext 供求值挂到节点的限定或投影
 *	表达式。另建第二个 ExprContext,
 *	即 rowcontext,在其中为每个 Values 子列表构建执行器
 *	表达式状态。重置该上下文可丢弃每行的表达式
 *	状态,避免长 VALUES 列表造成大量内存泄漏。
 *	但对含 SubPlan 的子列表不行,因为 SubPlan 须
 *	接到外层计划树才能正常工作。
 *	故仅对含 SubPlan 的子列表,在执行器启动时做
 *	表达式状态构建,并把那些指针存入
 *	exprstatelists[]。该数组中 NULL 项对应按上文
 *	所述处理的简单子表达式。
 * ----------------
 */
typedef struct ValuesScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	ExprContext *rowcontext;
	List	  **exprlists;
	List	  **exprstatelists;
	int			array_len;
	int			curr_idx;
} ValuesScanState;

/* ----------------
 *	TableFuncScanState
 *
 * 用于 XMLTABLE 等表表达式函数。
 * ----------------
 */
typedef struct TableFuncScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	ExprState  *docexpr;		/* 文档表达式的状态 */
	ExprState  *rowexpr;		/* 行生成表达式的状态 */
	List	   *colexprs;		/* 列生成表达式的状态 */
	List	   *coldefexprs;	/* 列默认表达式的状态 */
	List	   *ns_names;		/* 同 TableFunc.ns_names */
	List	   *ns_uris;		/* 名字空间 URI 表达式状态表 */
	Bitmapset  *notnulls;		/* 每输出列的可空标志 */
	void	   *opaque;			/* 表构建器私有空间 */
	const struct TableFuncRoutine *routine; /* 表构建器方法 */
	FmgrInfo   *in_functions;	/* 每列的输入函数 */
	Oid		   *typioparams;	/* 每列的 typioparam */
	int64		ordinal;		/* 下一个要输出的行号 */
	MemoryContext perTableCxt;	/* 每表上下文 */
	Tuplestorestate *tupstore;	/* 输出元组存储 */
} TableFuncScanState;

/* ----------------
 *	CteScanState 信息
 *
 *	CteScan 节点用于扫描 CommonTableExpr 查询。
 *
 * 多个 CteScan 节点可读同一 CTE 查询。我们用
 * tuplestore 保存已从 CTE 查询读出但尚未被全部
 * 读者消费的行。
 * ----------------
 */
typedef struct CteScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	int			eflags;			/* 传给 tuplestore 的能力标志 */
	int			readptr;		/* 我的 tuplestore 读指针下标 */
	PlanState  *cteplanstate;	/* CTE 查询本身的 PlanState */
	/* 链到"领导者" CteScanState(可能就是本节点) */
	struct CteScanState *leader;
	/* 其余字段仅在"领导者" CteScanState 中有效 */
	Tuplestorestate *cte_table; /* 已从 CTE 查询读出的行 */
	bool		eof_cte;		/* 已到 CTE 查询末尾? */
} CteScanState;

/* ----------------
 *	NamedTuplestoreScanState 信息
 *
 *	NamedTuplestoreScan 节点用于扫描在查询执行前已
 *	创建并命名的 Tuplestore。例如 AFTER 触发器的
 *	过渡表。
 *
 * 多个 NamedTuplestoreScan 节点可读同一 Tuplestore。
 * ----------------
 */
typedef struct NamedTuplestoreScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	int			readptr;		/* 我的 tuplestore 读指针下标 */
	TupleDesc	tupdesc;		/* tuplestore 中元组的格式 */
	Tuplestorestate *relation;	/* 诸行 */
} NamedTuplestoreScanState;

/* ----------------
 *	WorkTableScanState 信息
 *
 *	WorkTableScan 节点用于扫描 RecursiveUnion 节点
 *	创建的工作表。我们在执行器启动时定位
 *	RecursiveUnion 节点。
 * ----------------
 */
typedef struct WorkTableScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	RecursiveUnionState *rustate;
} WorkTableScanState;

/* ----------------
 *	ForeignScanState 信息
 *
 *	ForeignScan 节点用于扫描外表。
 * ----------------
 */
typedef struct ForeignScanState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	ExprState  *fdw_recheck_quals;	/* 不在 ss.ps.qual 的原限定 */
	Size		pscan_len;		/* 并行协调信息的大小 */
	ResultRelInfo *resultRelInfo;	/* 结果关系信息(UPDATE/DELETE) */
	/* 用结构指针以免在此包含 fdwapi.h */
	struct FdwRoutine *fdwroutine;
	void	   *fdw_state;		/* 外表包装器可在此保存状态 */
} ForeignScanState;

/* ----------------
 *	CustomScanState 信息
 *
 *	CustomScan 节点用于在执行器内跑定制代码。
 *
 * 核心代码切勿假定 CustomScanState 仅有此处声明
 * 的结构那么大;提供者可把它作为更大结构的首元素,
 * 通常也需要如此。该结构实际由计划节点关联的
 * CreateCustomScanState 方法分配。额外字段可在
 * 那里或在 BeginCustomScan 方法中初始化。
 * 见上。
 * ----------------
 */
struct CustomExecMethods;

typedef struct CustomScanState
{
	ScanState	ss;
	uint32		flags;			/* CUSTOMPATH_* 标志掩码,见
								 * nodes/extensible.h */
	List	   *custom_ps;		/* 子 PlanState 节点列表(若有) */
	Size		pscan_len;		/* 并行协调信息的大小 */
	const struct CustomExecMethods *methods;
	const struct TupleTableSlotOps *slotOps;
} CustomScanState;

/* ----------------------------------------------------------------
 *	连接状态信息
 * ----------------------------------------------------------------
 */

/* ----------------
 *	JoinState 信息
 *
 *	连接计划状态节点的超类。
 * ----------------
 */
typedef struct JoinState
{
	PlanState	ps;
	JoinType	jointype;
	bool		single_match;	/* 找到一个内层匹配后是否应跳到
								 * 下一个外层元组 */
	ExprState  *joinqual;		/* JOIN 限定(附加于 ps.qual) */
} JoinState;

/* ----------------
 *	NestLoopState 信息
 *
 *	NeedNewOuter	   下次调用需新外层元组则为真
 *	MatchedOuter	   当前外层元组已找到连接匹配则为真
 *	NullInnerTupleSlot 左外连接用的预置空元组
 * ----------------
 */
typedef struct NestLoopState
{
	JoinState	js;				/* 其首字段为 NodeTag */
	bool		nl_NeedNewOuter;
	bool		nl_MatchedOuter;
	TupleTableSlot *nl_NullInnerTupleSlot;
} NestLoopState;

/* ----------------
 *	MergeJoinState 信息
 *
 *	NumClauses		   可归并连接子句数
 *	Clauses			   每个可归并子句的信息
 *	JoinState		   ExecMergeJoin 状态机当前状态
 *	SkipMarkRestore    可否跳过 Mark 与 Restore 操作
 *	ExtraMarks		   是否对内层扫描发额外 Mark
 *	ConstFalseJoin	   连接限定为常量假则为真
 *	FillOuter		   是否仍发出未连接的外层元组
 *	FillInner		   是否仍发出未连接的内层元组
 *	MatchedOuter	   当前外层元组已找到连接匹配则为真
 *	MatchedInner	   当前内层元组已找到连接匹配则为真
 *	OuterTupleSlot	   元组表中当前外层元组的槽
 *	InnerTupleSlot	   元组表中当前内层元组的槽
 *	MarkedTupleSlot    元组表中已标记元组的槽
 *	NullOuterTupleSlot 右外连接用的预置空元组
 *	NullInnerTupleSlot 左外连接用的预置空元组
 *	OuterEContext	   计算外层元组连接值的工作区
 *	InnerEContext	   计算内层元组连接值的工作区
 * ----------------
 */
/* 私有于 nodeMergejoin.c: */
typedef struct MergeJoinClauseData *MergeJoinClause;

typedef struct MergeJoinState
{
	JoinState	js;				/* 其首字段为 NodeTag */
	int			mj_NumClauses;
	MergeJoinClause mj_Clauses; /* mj_NumClauses 长的数组 */
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
 *	HashJoinState 信息
 *
 *	hashclauses				哈希连接条件的原形式
 *	hj_OuterHashKeys		哈希连接条件中的外层哈希键
 *	hj_HashOperators		哈希连接条件中的连接运算符
 *	hj_HashTable			哈希连接的哈希表
 *							(尚未建表则为 NULL)
 *	hj_CurHashValue			当前外层元组的哈希值
 *	hj_CurBucketNo			当前外层元组的常规桶号
 *	hj_CurSkewBucketNo		当前外层元组的偏斜桶号
 *	hj_CurTuple				与当前外层匹配的上一内层元组
 *							(开始搜索则为 NULL)
 *	(若 OuterTupleSlot 为空则
 *	hj_CurXXX 未定义!)
 *	hj_OuterTupleSlot		外层元组的元组槽
 *	hj_HashTupleSlot		内层(已哈希)元组的元组槽
 *	hj_NullOuterTupleSlot	右/右反/全外连接用的预置空
 *							元组
 *	hj_NullInnerTupleSlot	左/全外连接用的预置空元组
 *	hj_FirstOuterTupleSlot	从外层计划取回的首个元组
 *	hj_JoinState			ExecHashJoin 状态机当前状态
 *	hj_MatchedOuter			当前外层已找到连接匹配则为真
 *	hj_OuterNotEmpty		外层关系已知非空则为真
 * ----------------
 */

/* 这些结构定义于 executor/hashjoin.h: */
typedef struct HashJoinTupleData *HashJoinTuple;
typedef struct HashJoinTableData *HashJoinTable;

typedef struct HashJoinState
{
	JoinState	js;				/* 其首字段为 NodeTag */
	ExprState  *hashclauses;
	List	   *hj_OuterHashKeys;	/* ExprState 节点列表 */
	List	   *hj_HashOperators;	/* 运算符 OID 列表 */
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
 *	物化状态信息
 * ----------------------------------------------------------------
 */

/* ----------------
 *	MaterialState 信息
 *
 *	物化节点用于把子计划结果物化到
 *	临时文件中。
 *
 *	ss.ss_ScanTupleSlot 引用底层计划的输出。
 * ----------------
 */
typedef struct MaterialState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	int			eflags;			/* 传给 tuplestore 的能力标志 */
	bool		eof_underlying; /* 是否已到底层计划末尾? */
	Tuplestorestate *tuplestorestate;
} MaterialState;

struct MemoizeEntry;
struct MemoizeTuple;
struct MemoizeKey;

typedef struct MemoizeInstrumentation
{
	uint64		cache_hits;		/* 找到扫描参数值可缓存的
								 * 重扫次数 */
	uint64		cache_misses;	/* 未找到扫描参数值可缓存的
								 * 重扫次数。 */
	uint64		cache_evictions;	/* 因需释放内存而移除的
									 * 缓存项数 */
	uint64		cache_overflows;	/* 因无法腾出足够空间存放当前
									 * 扫描元组而在填充缓存时
									 * 不得不绕过缓存的
									 * 次数。 */
	uint64		mem_peak;		/* 峰值内存用量(字节) */
} MemoizeInstrumentation;

/* ----------------
 *	每工作者 memoize 信息的共享内存容器
 * ----------------
 */
typedef struct SharedMemoizeInfo
{
	int			num_workers;
	MemoizeInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedMemoizeInfo;

/* ----------------
 *	MemoizeState 信息
 *
 *	memoize 节点用于缓存参数化扫描最近且常见的
 *	结果。
 * ----------------
 */
typedef struct MemoizeState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	int			mstatus;		/* ExecMemoize 状态机的值 */
	int			nkeys;			/* 缓存键个数 */
	struct memoize_hash *hashtable; /* 缓存项的哈希表 */
	TupleDesc	hashkeydesc;	/* 缓存键的元组描述符 */
	TupleTableSlot *tableslot;	/* 已有缓存项的最小元组槽 */
	TupleTableSlot *probeslot;	/* 供哈希查找的虚拟槽 */
	ExprState  *cache_eq_expr;	/* 比较执行参数与哈希键 */
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
 *	按多键排序时,输入数据集可能已按这些键的
 *	前缀排好序。我们称这些为
 *	"预排序键"。
 *	PresortedKeyData 表示关于这样一个键的信息。
 * ----------------
 */
typedef struct PresortedKeyData
{
	FmgrInfo	flinfo;			/* 比较函数信息 */
	FunctionCallInfo fcinfo;	/* 比较函数调用信息 */
	OffsetNumber attno;			/* 元组中的属性号 */
} PresortedKeyData;

/* ----------------
 *	每工作者排序信息的共享内存容器
 * ----------------
 */
typedef struct SharedSortInfo
{
	int			num_workers;
	TuplesortInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedSortInfo;

/* ----------------
 *	SortState 信息
 * ----------------
 */
typedef struct SortState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	bool		randomAccess;	/* 需对排序输出随机访问? */
	bool		bounded;		/* 结果集是否有界? */
	int64		bound;			/* 若有界,需要多少元组 */
	bool		sort_Done;		/* 排序已完成? */
	bool		bounded_Done;	/* 做排序时用的 bounded 值 */
	int64		bound_Done;		/* 做排序时用的 bound 值 */
	void	   *tuplesortstate; /* tuplesort.c 的私有状态 */
	bool		am_worker;		/* 是工作者? */
	bool		datumSort;		/* Datum 排序而非元组排序? */
	SharedSortInfo *shared_info;	/* 每工作者一项 */
} SortState;

/* ----------------
 *	IncrementalSort 的插桩信息
 * ----------------
 */
typedef struct IncrementalSortGroupInfo
{
	int64		groupCount;
	int64		maxDiskSpaceUsed;
	int64		totalDiskSpaceUsed;
	int64		maxMemorySpaceUsed;
	int64		totalMemorySpaceUsed;
	bits32		sortMethods;	/* TuplesortMethod 位掩码 */
} IncrementalSortGroupInfo;

typedef struct IncrementalSortInfo
{
	IncrementalSortGroupInfo fullsortGroupInfo;
	IncrementalSortGroupInfo prefixsortGroupInfo;
} IncrementalSortInfo;

/* ----------------
 *	每工作者增量排序信息的共享内存容器
 * ----------------
 */
typedef struct SharedIncrementalSortInfo
{
	int			num_workers;
	IncrementalSortInfo sinfo[FLEXIBLE_ARRAY_MEMBER];
} SharedIncrementalSortInfo;

/* ----------------
 *	IncrementalSortState 信息
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
	ScanState	ss;				/* 其首字段为 NodeTag */
	bool		bounded;		/* 结果集是否有界? */
	int64		bound;			/* 若有界,需要多少元组 */
	bool		outerNodeDone;	/* 是否已从外层节点取完元组 */
	int64		bound_Done;		/* 做排序时用的 bound 值 */
	IncrementalSortExecutionStatus execution_status;
	int64		n_fullsort_remaining;
	Tuplesortstate *fullsort_state; /* tuplesort.c 的私有状态 */
	Tuplesortstate *prefixsort_state;	/* tuplesort.c 的私有状态 */
	/* 输入路径已按之排序的键 */
	PresortedKeyData *presorted_keys;

	IncrementalSortInfo incsort_info;

	/* 组内预排序键取值所依的枢轴元组槽 */
	TupleTableSlot *group_pivot;
	TupleTableSlot *transfer_tuple;
	bool		am_worker;		/* 是工作者? */
	SharedIncrementalSortInfo *shared_info; /* 每工作者一项 */
} IncrementalSortState;

/* ---------------------
 *	GroupState 信息
 * ---------------------
 */
typedef struct GroupState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	ExprState  *eqfunction;		/* 相等函数 */
	bool		grp_done;		/* 指示 Group 扫描完成 */
} GroupState;

/* ---------------------
 *	每工作者聚合信息
 * ---------------------
 */
typedef struct AggregateInstrumentation
{
	Size		hash_mem_peak;	/* 哈希表峰值内存用量 */
	uint64		hash_disk_used; /* 磁盘用量(kB) */
	int			hash_batches_used;	/* 整个执行期间用过的批次数 */
} AggregateInstrumentation;

/* ----------------
 *	每工作者聚合信息的共享内存容器
 * ----------------
 */
typedef struct SharedAggInfo
{
	int			num_workers;
	AggregateInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedAggInfo;

/* ---------------------
 *	AggState 信息
 *
 *	ss.ss_ScanTupleSlot 引用底层计划的输出。
 *
 *	注: ss.ps.ps_ExprContext 含 ecxt_aggvalues 与
 *	ecxt_aggnulls 数组,在求值 Agg 节点输出元组期间
 *	保存当前输入组的已计算聚合值。我们另建第二
 *	ExprContext tmpcontext,用于求值输入表达式并运行
 *	聚合转移函数。
 * ---------------------
 */
/* 这些结构私有于 nodeAgg.c: */
typedef struct AggStatePerAggData *AggStatePerAgg;
typedef struct AggStatePerTransData *AggStatePerTrans;
typedef struct AggStatePerGroupData *AggStatePerGroup;
typedef struct AggStatePerPhaseData *AggStatePerPhase;
typedef struct AggStatePerHashData *AggStatePerHash;

typedef struct AggState
{
	ScanState	ss;				/* 其首字段为 NodeTag */
	List	   *aggs;			/* tlist 与限定中全部 Aggref */
	int			numaggs;		/* 列表长度(可为零!) */
	int			numtrans;		/* pertrans 项个数 */
	AggStrategy aggstrategy;	/* 策略模式 */
	AggSplit	aggsplit;		/* 聚合拆分模式,见 nodes.h */
	AggStatePerPhase phase;		/* 指向当前阶段数据 */
	int			numphases;		/* 阶段数(含阶段 0) */
	int			current_phase;	/* 当前阶段号 */
	AggStatePerAgg peragg;		/* 每 Aggref 信息 */
	AggStatePerTrans pertrans;	/* 每 Trans 状态信息 */
	ExprContext *hashcontext;	/* 长寿命数据的 econtext(哈希表) */
	ExprContext **aggcontexts;	/* 长寿命数据的 econtext(每 GS) */
	ExprContext *tmpcontext;	/* 输入表达式的 econtext */
#define FIELDNO_AGGSTATE_CURAGGCONTEXT 14
	ExprContext *curaggcontext; /* 当前活动的 aggcontext */
	AggStatePerAgg curperagg;	/* 当前活动的聚合(若有) */
#define FIELDNO_AGGSTATE_CURPERTRANS 16
	AggStatePerTrans curpertrans;	/* 当前活动的转移状态(若有) */
	bool		input_done;		/* 指示输入结束 */
	bool		agg_done;		/* 指示 Agg 扫描完成 */
	int			projected_set;	/* 上次投影的分组集 */
#define FIELDNO_AGGSTATE_CURRENT_SET 20
	int			current_set;	/* 正在求值的当前分组集 */
	Bitmapset  *grouped_cols;	/* 当前投影中的分组列 */
	List	   *all_grouped_cols;	/* 全部已分组列的列表(降序) */
	Bitmapset  *colnos_needed;	/* 外层计划所需的全部列 */
	int			max_colno_needed;	/* 外层计划所需的最高列号 */
	bool		all_cols_needed;	/* 是否需外层计划全部列? */
	/* 这些字段供分组集阶段数据 */
	int			maxsets;		/* 任一阶段中的最大集合数 */
	AggStatePerPhase phases;	/* 全部阶段数组 */
	Tuplesortstate *sort_in;	/* 阶段>1 已排序输入 */
	Tuplesortstate *sort_out;	/* 输入复制到此供下一阶段 */
	TupleTableSlot *sort_slot;	/* 排序结果的槽 */
	/* 下列字段用于 AGG_PLAIN 与 AGG_SORTED 模式: */
	AggStatePerGroup *pergroups;	/* 按分组集索引的每组
									 * 指针 */
	HeapTuple	grp_firstTuple; /* 当前组首个元组的拷贝 */
	/* 下列字段用于 AGG_HASHED 与 AGG_MIXED 模式: */
	bool		table_filled;	/* 哈希表已填满? */
	int			num_hashes;
	MemoryContext hash_metacxt; /* 哈希表本身的内存 */
	struct LogicalTapeSet *hash_tapeset;	/* 哈希溢出用的磁带集 */
	struct HashAggSpill *hash_spills;	/* 每个分组集的 HashAggSpill,
										 * 仅在第一遍存在 */
	TupleTableSlot *hash_spill_rslot;	/* 供读溢出文件 */
	TupleTableSlot *hash_spill_wslot;	/* 供写溢出文件 */
	List	   *hash_batches;	/* 尚待处理的哈希批次数 */
	bool		hash_ever_spilled;	/* 本次执行是否曾溢出? */
	bool		hash_spill_mode;	/* 当前批次碰到了限制且
									 * 不得再创建新组 */
	Size		hash_mem_limit; /* 溢出哈希表前的限额 */
	uint64		hash_ngroups_limit; /* 溢出哈希表前的限额 */
	int			hash_planned_partitions;	/* 第一遍计划的
											 * 分区数 */
	double		hashentrysize;	/* 执行期间修订的估计 */
	Size		hash_mem_peak;	/* 哈希表峰值内存用量 */
	uint64		hash_ngroups_current;	/* number of groups currently in
										 * memory in all hash tables */
	uint64		hash_disk_used; /* 磁盘用量(kB) */
	int			hash_batches_used;	/* 整个执行期间用过的批次数 */

	AggStatePerHash perhash;	/* 每哈希表数据的数组 */
	AggStatePerGroup *hash_pergroup;	/* 按分组集索引的
										 * 每组指针数组 */

	/* 支持求值聚合输入表达式: */
#define FIELDNO_AGGSTATE_ALL_PERGROUPS 53
	AggStatePerGroup *all_pergroups;	/* 先是 ->pergroups,再是
										 * ->hash_pergroup */
	ProjectionInfo *combinedproj;	/* 投影机制 */
	SharedAggInfo *shared_info; /* 每工作者一项 */
} AggState;

/* ----------------
 *	WindowAggState 信息
 * ----------------
 */
/* 这些结构私有于 nodeWindowAgg.c: */
typedef struct WindowStatePerFuncData *WindowStatePerFunc;
typedef struct WindowStatePerAggData *WindowStatePerAgg;

/*
 * WindowAggStatus -- 用于跟踪 WindowAggState 的状态
 */
typedef enum WindowAggStatus
{
	WINDOWAGG_DONE,				/* 无更多处理要做 */
	WINDOWAGG_RUN,				/* 窗口函数的正常处理 */
	WINDOWAGG_PASSTHROUGH,		/* 不求值窗口函数 */
	WINDOWAGG_PASSTHROUGH_STRICT	/* 直通且在 spool 期间不存
									 * 新元组 */
} WindowAggStatus;

typedef struct WindowAggState
{
	ScanState	ss;				/* 其首字段为 NodeTag */

	/* 下列字段由 ExecInitExpr 填写: */
	List	   *funcs;			/* tlist 中全部 WindowFunc */
	int			numfuncs;		/* 窗口函数总数 */
	int			numaggs;		/* 其中普通聚合的个数 */

	WindowStatePerFunc perfunc; /* 每窗口函数信息 */
	WindowStatePerAgg peragg;	/* 每普通聚合信息 */
	ExprState  *partEqfunction; /* 分区列的相等函数 */
	ExprState  *ordEqfunction;	/* 排序列的相等函数 */
	Tuplestorestate *buffer;	/* 存放当前分区的行 */
	int			current_ptr;	/* 当前行的读指针号 */
	int			framehead_ptr;	/* 帧头读指针号(若用) */
	int			frametail_ptr;	/* 帧尾读指针号(若用) */
	int			grouptail_ptr;	/* 组尾读指针号(若用) */
	int64		spooled_rows;	/* 缓冲区中的总行数 */
	int64		currentpos;		/* 当前行在分区中的位置 */
	int64		frameheadpos;	/* 当前帧头位置 */
	int64		frametailpos;	/* 当前帧尾位置(帧末+1) */
	/* 用结构指针以免在此包含 windowapi.h */
	struct WindowObjectData *agg_winobj;	/* 供聚合取数的 winobj */
	int64		aggregatedbase; /* 当前聚合的起始行 */
	int64		aggregatedupto; /* 此行之前的行已聚合 */
	WindowAggStatus status;		/* WindowAggState 运行状态 */

	int			frameOptions;	/* frame_clause 选项,见 WindowDef */
	ExprState  *startOffset;	/* 起始边界偏移的表达式 */
	ExprState  *endOffset;		/* 结束边界偏移的表达式 */
	Datum		startOffsetValue;	/* startOffset 求值结果 */
	Datum		endOffsetValue; /* endOffset 求值结果 */

	/* 下列字段用于 RANGE 偏移 PRECEDING/FOLLOWING: */
	FmgrInfo	startInRangeFunc;	/* startOffset 的 in_range 函数 */
	FmgrInfo	endInRangeFunc; /* endOffset 的 in_range 函数 */
	Oid			inRangeColl;	/* in_range 测试排序规则 */
	bool		inRangeAsc;		/* in_range 测试用升序? */
	bool		inRangeNullsFirst;	/* in_range 测试空值排前? */

	/* 下列字段用于 GROUPS 模式: */
	int64		currentgroup;	/* 当前行在分区中的对等组号 */
	int64		frameheadgroup; /* 帧头行的对等组号 */
	int64		frametailgroup; /* 帧尾行的对等组号 */
	int64		groupheadpos;	/* 当前行对等组的头位置 */
	int64		grouptailpos;	/* 同上尾位置(组末+1) */

	MemoryContext partcontext;	/* 分区寿命数据的上下文 */
	MemoryContext aggcontext;	/* 聚合工作数据的共享上下文 */
	MemoryContext curaggcontext;	/* 当前聚合的工作数据 */
	ExprContext *tmpcontext;	/* 短期求值上下文 */

	ExprState  *runcondition;	/* 必须保持为真的条件,否则
								 * WindowAgg 执行将结束或
								 * 进入直通模式。无此条件时为
								 * NULL。 */

	bool		use_pass_through;	/* When false, stop execution when
									 * runcondition is no longer true.  Else
									 * just stop evaluating window funcs. */
	bool		top_window;		/* 这是最顶层 WindowAgg 或本查询级
								 * 唯一 WindowAgg 则为真 */
	bool		all_first;		/* 扫描正在启动则为真 */
	bool		partition_spooled;	/* 当前分区全部元组已 spool 进
									 * tuplestore 则为真 */
	bool		more_partitions;	/* 本分区之后还有分区则为
									 * 真 */
	bool		framehead_valid;	/* true if frameheadpos is known up to
									 * date for current row */
	bool		frametail_valid;	/* true if frametailpos is known up to
									 * date for current row */
	bool		grouptail_valid;	/* true if grouptailpos is known up to
									 * date for current row */

	TupleTableSlot *first_part_slot;	/* 当前或下一分区的
										 * 首元组 */
	TupleTableSlot *framehead_slot; /* 当前帧的首个元组 */
	TupleTableSlot *frametail_slot; /* 当前帧之后的首个元组 */

	/* 从 tuplestore 取回元组的临时槽 */
	TupleTableSlot *agg_row_slot;
	TupleTableSlot *temp_slot_1;
	TupleTableSlot *temp_slot_2;
} WindowAggState;

/* ----------------
 *	UniqueState 信息
 *
 *	Unique 节点用在排序节点"之上"以丢弃
 *	排序阶段返回的重复元组。基本上它只是
 *	把子计划当前元组与先前取回的元组
 *	(存在其结果槽中)比较。
 *	若两者在全部相关字段上相同,则再从
 *	排序取下一个元组并重试。
 * ----------------
 */
typedef struct UniqueState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	ExprState  *eqfunction;		/* 元组相等限定 */
} UniqueState;

/* ----------------
 * GatherState 信息
 *
 *	Gather 节点启动 1 个或多个并行工作者,在那些
 *	工作者中跑子计划并收集结果。
 * ----------------
 */
typedef struct GatherState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	bool		initialized;	/* 工作者启动? */
	bool		need_to_scan_locally;	/* 需从本地计划读取? */
	int64		tuples_needed;	/* 元组界限,见 ExecSetTupleBound */
	/* 下列字段只设置一次: */
	TupleTableSlot *funnel_slot;
	struct ParallelExecutorInfo *pei;
	/* 其余字段在重扫时全部重新初始化: */
	int			nworkers_launched;	/* 原工作者数 */
	int			nreaders;		/* 仍活动的工作者数 */
	int			nextreader;		/* 下一个尝试读取的 */
	struct TupleQueueReader **reader;	/* 有 nreaders 个活动项的数组 */
} GatherState;

/* ----------------
 * GatherMergeState 信息
 *
 *	GatherMerge 节点启动 1 个或多个并行工作者,在
 *	每工作者中跑产生已排序输出的子计划,然后把
 *	结果归并成单一已排序流。
 * ----------------
 */
struct GMReaderTupleBuffer;		/* 私有于 nodeGatherMerge.c */

typedef struct GatherMergeState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	bool		initialized;	/* 工作者启动? */
	bool		gm_initialized; /* gather_merge_init() done? */
	bool		need_to_scan_locally;	/* 需从本地计划读取? */
	int64		tuples_needed;	/* 元组界限,见 ExecSetTupleBound */
	/* 下列字段只设置一次: */
	TupleDesc	tupDesc;		/* 子计划结果元组的描述符 */
	int			gm_nkeys;		/* 排序列数 */
	SortSupport gm_sortkeys;	/* gm_nkeys 长的数组 */
	struct ParallelExecutorInfo *pei;
	/* 其余字段在重扫时全部重新初始化 */
	/* (但数组不重新分配,只清空) */
	int			nworkers_launched;	/* 原工作者数 */
	int			nreaders;		/* 活动工作者数 */
	TupleTableSlot **gm_slots;	/* 有 nreaders+1 项的数组 */
	struct TupleQueueReader **reader;	/* 有 nreaders 个活动项的数组 */
	struct GMReaderTupleBuffer *gm_tuple_buffers;	/* nreaders 元组缓冲 */
	struct binaryheap *gm_heap; /* 槽下标的二叉堆 */
} GatherMergeState;

/* ----------------
 *	EXPLAIN ANALYZE 显示的值
 * ----------------
 */
typedef struct HashInstrumentation
{
	int			nbuckets;		/* 执行结束时的桶数 */
	int			nbuckets_original;	/* 计划的桶数 */
	int			nbatch;			/* 执行结束时的批次数 */
	int			nbatch_original;	/* 计划的批次数 */
	Size		space_peak;		/* 峰值内存用量(字节) */
} HashInstrumentation;

/* ----------------
 *	每工作者哈希信息的共享内存容器
 * ----------------
 */
typedef struct SharedHashInfo
{
	int			num_workers;
	HashInstrumentation hinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedHashInfo;

/* ----------------
 *	HashState 信息
 * ----------------
 */
typedef struct HashState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	HashJoinTable hashtable;	/* hashjoin 的哈希表 */
	List	   *hashkeys;		/* ExprState 节点列表 */

	/*
	 * 在并行化哈希连接中,领导者在 shared_info 字段中
	 * 保留共享内存统计区指针,并在 DSM 关闭前把共享
	 * 内存信息拷回本地存储。工作者或非并行连接中
	 * shared_info 字段保持为 NULL。
	 */
	SharedHashInfo *shared_info;

	/*
	 * 若在收集哈希统计,这指向初始为零的收集区,
	 * 可以是本地存储或共享内存;无论哪种都只供
	 * 一个进程使用。
	 */
	HashInstrumentation *hinstrument;

	/* 并行哈希状态 */
	struct ParallelHashJoinState *parallel_state;
} HashState;

/* ----------------
 *	SetOpState 信息
 *
 *	即使在"已排序"模式下,SetOp 节点也比简单 Unique
 *	更复杂,因为须统计要返回多少重复。但我们也支持
 *	哈希,故这其实更像是 Agg 的精简
 *	形式。
 * ----------------
 */
/* 本结构私有于 nodeSetOp.c: */
typedef struct SetOpStatePerGroupData *SetOpStatePerGroup;

typedef struct SetOpState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	ExprState  *eqfunction;		/* 相等比较器 */
	Oid		   *eqfuncoids;		/* 每分组字段的相等函数 */
	FmgrInfo   *hashfunctions;	/* 每分组字段哈希函数 */
	bool		setop_done;		/* 指示输出扫描完成 */
	long		numOutput;		/* 尚待输出的重复数 */
	/* 下列字段用于 SETOP_SORTED 模式: */
	SetOpStatePerGroup pergroup;	/* 每组工作状态 */
	HeapTuple	grp_firstTuple; /* 当前组首个元组的拷贝 */
	/* 下列字段用于 SETOP_HASHED 模式: */
	TupleHashTable hashtable;	/* 每组一项的哈希表 */
	MemoryContext tableContext; /* 含哈希表的内存上下文 */
	bool		table_filled;	/* 哈希表已填满? */
	TupleHashIterator hashiter; /* 供遍历哈希表 */
} SetOpState;

/* ----------------
 *	LockRowsState 信息
 *
 *	LockRows 节点用于强制 FOR [KEY] UPDATE/SHARE 加锁。
 * ----------------
 */
typedef struct LockRowsState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	List	   *lr_arowMarks;	/* ExecAuxRowMarks 列表 */
	EPQState	lr_epqstate;	/* 供求值 EvalPlanQual 重检 */
} LockRowsState;

/* ----------------
 *	LimitState 信息
 *
 *	Limit 节点用于强制 LIMIT/OFFSET 子句。
 *	它们只选取子计划输出中所需的子范围。
 *
 * offset 是要跳过的初始元组数(0 无操作)。
 * count 是跳过 offset 个元组后要返回的元组数。
 * 若未指定限量,count 未定义且 noCount 为真。
 * 当 lstate == LIMIT_INITIAL 时,offset/count/noCount 尚未设置。
 * ----------------
 */
typedef enum
{
	LIMIT_INITIAL,				/* LIMIT 节点的初始状态 */
	LIMIT_RESCAN,				/* 重算参数后重扫 */
	LIMIT_EMPTY,				/* 没有可返回的行 */
	LIMIT_INWINDOW,				/* 窗口内已返回过一行 */
	LIMIT_WINDOWEND_TIES,		/* 已返回过并列行 */
	LIMIT_SUBPLANEOF,			/* 子计划已到 EOF(窗口内) */
	LIMIT_WINDOWEND,			/* 已走出窗口末端 */
	LIMIT_WINDOWSTART			/* 已走出窗口开端 */
} LimitStateCond;

typedef struct LimitState
{
	PlanState	ps;				/* 其首字段为 NodeTag */
	ExprState  *limitOffset;	/* OFFSET 参数,无则为 NULL */
	ExprState  *limitCount;		/* COUNT 参数,无则为 NULL */
	LimitOption limitOption;	/* 限量规格类型 */
	int64		offset;			/* 当前 OFFSET 值 */
	int64		count;			/* 当前 COUNT(若有) */
	bool		noCount;		/* 为真则忽略 count */
	LimitStateCond lstate;		/* 状态机状态,见上 */
	int64		position;		/* 上次返回元组的 1 基下标 */
	TupleTableSlot *subSlot;	/* 上次取自子计划的元组 */
	ExprState  *eqfunction;		/* WITH TIES 选项时的元组相等
								 * 限定 */
	TupleTableSlot *last_slot;	/* 求值并列的槽 */
} LimitState;

#endif							/* EXECNODES_H */
