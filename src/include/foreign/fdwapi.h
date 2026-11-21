/*-------------------------------------------------------------------------
 *
 * fdwapi.h
 *	  外部数据包装器的 API
 *
 * Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/foreign/fdwapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef FDWAPI_H
#define FDWAPI_H

#include "access/parallel.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"

/* 为免此处包含 explain.h，如此引用 ExplainState： */
struct ExplainState;


/*
 * 回调函数签名 —— 详见 fdwhandler.sgml。
 */

typedef void (*GetForeignRelSize_function) (PlannerInfo *root,
											RelOptInfo *baserel,
											Oid foreigntableid);

typedef void (*GetForeignPaths_function) (PlannerInfo *root,
										  RelOptInfo *baserel,
										  Oid foreigntableid);

typedef ForeignScan *(*GetForeignPlan_function) (PlannerInfo *root,
												 RelOptInfo *baserel,
												 Oid foreigntableid,
												 ForeignPath *best_path,
												 List *tlist,
												 List *scan_clauses,
												 Plan *outer_plan);

typedef void (*BeginForeignScan_function) (ForeignScanState *node,
										   int eflags);

typedef TupleTableSlot *(*IterateForeignScan_function) (ForeignScanState *node);

typedef bool (*RecheckForeignScan_function) (ForeignScanState *node,
											 TupleTableSlot *slot);

typedef void (*ReScanForeignScan_function) (ForeignScanState *node);

typedef void (*EndForeignScan_function) (ForeignScanState *node);

typedef void (*GetForeignJoinPaths_function) (PlannerInfo *root,
											  RelOptInfo *joinrel,
											  RelOptInfo *outerrel,
											  RelOptInfo *innerrel,
											  JoinType jointype,
											  JoinPathExtraData *extra);

typedef void (*GetForeignUpperPaths_function) (PlannerInfo *root,
											   UpperRelationKind stage,
											   RelOptInfo *input_rel,
											   RelOptInfo *output_rel,
											   void *extra);

typedef void (*AddForeignUpdateTargets_function) (PlannerInfo *root,
												  Index rtindex,
												  RangeTblEntry *target_rte,
												  Relation target_relation);

typedef List *(*PlanForeignModify_function) (PlannerInfo *root,
											 ModifyTable *plan,
											 Index resultRelation,
											 int subplan_index);

typedef void (*BeginForeignModify_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo,
											 List *fdw_private,
											 int subplan_index,
											 int eflags);

typedef TupleTableSlot *(*ExecForeignInsert_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef TupleTableSlot **(*ExecForeignBatchInsert_function) (EState *estate,
															 ResultRelInfo *rinfo,
															 TupleTableSlot **slots,
															 TupleTableSlot **planSlots,
															 int *numSlots);

typedef int (*GetForeignModifyBatchSize_function) (ResultRelInfo *rinfo);

typedef TupleTableSlot *(*ExecForeignUpdate_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef TupleTableSlot *(*ExecForeignDelete_function) (EState *estate,
													   ResultRelInfo *rinfo,
													   TupleTableSlot *slot,
													   TupleTableSlot *planSlot);

typedef void (*EndForeignModify_function) (EState *estate,
										   ResultRelInfo *rinfo);

typedef void (*BeginForeignInsert_function) (ModifyTableState *mtstate,
											 ResultRelInfo *rinfo);

typedef void (*EndForeignInsert_function) (EState *estate,
										   ResultRelInfo *rinfo);

typedef int (*IsForeignRelUpdatable_function) (Relation rel);

typedef bool (*PlanDirectModify_function) (PlannerInfo *root,
										   ModifyTable *plan,
										   Index resultRelation,
										   int subplan_index);

typedef void (*BeginDirectModify_function) (ForeignScanState *node,
											int eflags);

typedef TupleTableSlot *(*IterateDirectModify_function) (ForeignScanState *node);

typedef void (*EndDirectModify_function) (ForeignScanState *node);

typedef RowMarkType (*GetForeignRowMarkType_function) (RangeTblEntry *rte,
													   LockClauseStrength strength);

typedef void (*RefetchForeignRow_function) (EState *estate,
											ExecRowMark *erm,
											Datum rowid,
											TupleTableSlot *slot,
											bool *updated);

typedef void (*ExplainForeignScan_function) (ForeignScanState *node,
											 struct ExplainState *es);

typedef void (*ExplainForeignModify_function) (ModifyTableState *mtstate,
											   ResultRelInfo *rinfo,
											   List *fdw_private,
											   int subplan_index,
											   struct ExplainState *es);

typedef void (*ExplainDirectModify_function) (ForeignScanState *node,
											  struct ExplainState *es);

typedef int (*AcquireSampleRowsFunc) (Relation relation, int elevel,
									  HeapTuple *rows, int targrows,
									  double *totalrows,
									  double *totaldeadrows);

typedef bool (*AnalyzeForeignTable_function) (Relation relation,
											  AcquireSampleRowsFunc *func,
											  BlockNumber *totalpages);

typedef List *(*ImportForeignSchema_function) (ImportForeignSchemaStmt *stmt,
											   Oid serverOid);

typedef void (*ExecForeignTruncate_function) (List *rels,
											  DropBehavior behavior,
											  bool restart_seqs);

typedef Size (*EstimateDSMForeignScan_function) (ForeignScanState *node,
												 ParallelContext *pcxt);
typedef void (*InitializeDSMForeignScan_function) (ForeignScanState *node,
												   ParallelContext *pcxt,
												   void *coordinate);
typedef void (*ReInitializeDSMForeignScan_function) (ForeignScanState *node,
													 ParallelContext *pcxt,
													 void *coordinate);
typedef void (*InitializeWorkerForeignScan_function) (ForeignScanState *node,
													  shm_toc *toc,
													  void *coordinate);
typedef void (*ShutdownForeignScan_function) (ForeignScanState *node);
typedef bool (*IsForeignScanParallelSafe_function) (PlannerInfo *root,
													RelOptInfo *rel,
													RangeTblEntry *rte);
typedef List *(*ReparameterizeForeignPathByChild_function) (PlannerInfo *root,
															List *fdw_private,
															RelOptInfo *child_rel);

typedef bool (*IsForeignPathAsyncCapable_function) (ForeignPath *path);

typedef void (*ForeignAsyncRequest_function) (AsyncRequest *areq);

typedef void (*ForeignAsyncConfigureWait_function) (AsyncRequest *areq);

typedef void (*ForeignAsyncNotify_function) (AsyncRequest *areq);

/*
 * FdwRoutine 是外部数据包装器处理器函数返回的
 * 结构。它提供规划器与执行器所需回调函数的
 * 指针。
 *
 * 将来可能增加更多函数指针。故建议处理器用
 * makeNode(FdwRoutine) 初始化该结构，使全部字段
 * 为 NULL。这将确保无字段意外未定义。
 * 。
 */
typedef struct FdwRoutine
{
	NodeTag		type;

	/* 扫描外部表的函数 */
	GetForeignRelSize_function GetForeignRelSize;
	GetForeignPaths_function GetForeignPaths;
	GetForeignPlan_function GetForeignPlan;
	BeginForeignScan_function BeginForeignScan;
	IterateForeignScan_function IterateForeignScan;
	ReScanForeignScan_function ReScanForeignScan;
	EndForeignScan_function EndForeignScan;

	/*
	 * 其余函数可选。未提供者指针置 NULL。
	 * 。
	 */

	/* 远程连接规划的函数 */
	GetForeignJoinPaths_function GetForeignJoinPaths;

	/* 远程上层关系（扫描/连接后）规划的函数 */
	GetForeignUpperPaths_function GetForeignUpperPaths;

	/* 更新外部表的函数 */
	AddForeignUpdateTargets_function AddForeignUpdateTargets;
	PlanForeignModify_function PlanForeignModify;
	BeginForeignModify_function BeginForeignModify;
	ExecForeignInsert_function ExecForeignInsert;
	ExecForeignBatchInsert_function ExecForeignBatchInsert;
	GetForeignModifyBatchSize_function GetForeignModifyBatchSize;
	ExecForeignUpdate_function ExecForeignUpdate;
	ExecForeignDelete_function ExecForeignDelete;
	EndForeignModify_function EndForeignModify;
	BeginForeignInsert_function BeginForeignInsert;
	EndForeignInsert_function EndForeignInsert;
	IsForeignRelUpdatable_function IsForeignRelUpdatable;
	PlanDirectModify_function PlanDirectModify;
	BeginDirectModify_function BeginDirectModify;
	IterateDirectModify_function IterateDirectModify;
	EndDirectModify_function EndDirectModify;

	/* SELECT FOR UPDATE/SHARE 行锁的函数 */
	GetForeignRowMarkType_function GetForeignRowMarkType;
	RefetchForeignRow_function RefetchForeignRow;
	RecheckForeignScan_function RecheckForeignScan;

	/* EXPLAIN 支持函数 */
	ExplainForeignScan_function ExplainForeignScan;
	ExplainForeignModify_function ExplainForeignModify;
	ExplainDirectModify_function ExplainDirectModify;

	/* ANALYZE 支持函数 */
	AnalyzeForeignTable_function AnalyzeForeignTable;

	/* IMPORT FOREIGN SCHEMA 支持函数 */
	ImportForeignSchema_function ImportForeignSchema;

	/* TRUNCATE 支持函数 */
	ExecForeignTruncate_function ExecForeignTruncate;

	/* Gather 节点下并行的支持函数 */
	IsForeignScanParallelSafe_function IsForeignScanParallelSafe;
	EstimateDSMForeignScan_function EstimateDSMForeignScan;
	InitializeDSMForeignScan_function InitializeDSMForeignScan;
	ReInitializeDSMForeignScan_function ReInitializeDSMForeignScan;
	InitializeWorkerForeignScan_function InitializeWorkerForeignScan;
	ShutdownForeignScan_function ShutdownForeignScan;

	/* 路径重参数化的支持函数。 */
	ReparameterizeForeignPathByChild_function ReparameterizeForeignPathByChild;

	/* 异步执行的支持函数 */
	IsForeignPathAsyncCapable_function IsForeignPathAsyncCapable;
	ForeignAsyncRequest_function ForeignAsyncRequest;
	ForeignAsyncConfigureWait_function ForeignAsyncConfigureWait;
	ForeignAsyncNotify_function ForeignAsyncNotify;
} FdwRoutine;


/* foreign/foreign.c 中的函数 */
extern FdwRoutine *GetFdwRoutine(Oid fdwhandler);
extern Oid	GetForeignServerIdByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineByServerId(Oid serverid);
extern FdwRoutine *GetFdwRoutineByRelId(Oid relid);
extern FdwRoutine *GetFdwRoutineForRelation(Relation relation, bool makecopy);
extern bool IsImportableForeignTable(const char *tablename,
									 ImportForeignSchemaStmt *stmt);
extern Path *GetExistingLocalJoinPath(RelOptInfo *joinrel);

#endif							/* FDWAPI_H */
