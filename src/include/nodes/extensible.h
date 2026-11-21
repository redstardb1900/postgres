/*-------------------------------------------------------------------------
 *
 * extensible.h
 *	可扩展节点与自定义扫描的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/extensible.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENSIBLE_H
#define EXTENSIBLE_H

#include "access/parallel.h"
#include "commands/explain.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

/* 可扩展节点标识符的最大长度 */
#define EXTNODENAME_MAX_LEN					64

/*
 * 可扩展节点是由扩展定义的新节点类型。类型恒为
 * T_ExtensibleNode,而 extnodename 标识具体节点种类。
 * 可查 extnodename 找到该节点类型的
 * ExtensibleNodeMethods。
 */
typedef struct ExtensibleNode
{
	pg_node_attr(custom_copy_equal, custom_read_write)

	NodeTag		type;
	const char *extnodename;	/* ExtensibleNodeMethods 的标识符 */
} ExtensibleNode;

/*
 * node_size 是此类型可扩展节点的字节大小。
 *
 * nodeCopy 从 oldnode 深拷到 newnode。
 * 不必拷 type 或 extnodename,由核心系统拷贝。
 * 见上。
 *
 * nodeEqual 对 a 与 b 做深相等比较并返回真假。
 * 不必比较 type 或 extnodename,由核心系统比较。
 * 见上。
 *
 * nodeOut 是该节点类型的序列化函数。应遵循
 * outfuncs.c 典型输出约定。不必输出 type 或
 * extnodename；核心系统处理它们。
 *
 * nodeRead 是反序列化函数。不必读 type 或
 * extnodename；核心系统处理。应用 pg_strtok()
 * 从当前输入流取下一记号,再按 readfuncs.c 方式
 * 重建私有字段。
 *
 * 所有回调均为必需。
 */
typedef struct ExtensibleNodeMethods
{
	const char *extnodename;
	Size		node_size;
	void		(*nodeCopy) (struct ExtensibleNode *newnode,
							 const struct ExtensibleNode *oldnode);
	bool		(*nodeEqual) (const struct ExtensibleNode *a,
							  const struct ExtensibleNode *b);
	void		(*nodeOut) (struct StringInfoData *str,
							const struct ExtensibleNode *node);
	void		(*nodeRead) (struct ExtensibleNode *node);
} ExtensibleNodeMethods;

extern void RegisterExtensibleNodeMethods(const ExtensibleNodeMethods *methods);
extern const ExtensibleNodeMethods *GetExtensibleNodeMethods(const char *extnodename,
															 bool missing_ok);

/*
 * 自定义路径标志,表示结果扫描将具备的能力。
 * CustomPath 与 CustomScan 节点的 flags 字段是这些
 * 标志的位掩码。
 */
#define CUSTOMPATH_SUPPORT_BACKWARD_SCAN	0x0001
#define CUSTOMPATH_SUPPORT_MARK_RESTORE		0x0002
#define CUSTOMPATH_SUPPORT_PROJECTION		0x0004

/*
 * 自定义路径方法。大体只需知如何把 CustomPath
 * 转为计划。
 */
typedef struct CustomPathMethods
{
	const char *CustomName;

	/* 把 Path 转为 Plan */
	struct Plan *(*PlanCustomPath) (PlannerInfo *root,
									RelOptInfo *rel,
									struct CustomPath *best_path,
									List *tlist,
									List *clauses,
									List *custom_plans);
	struct List *(*ReparameterizeCustomPathByChild) (PlannerInfo *root,
													 List *custom_private,
													 RelOptInfo *child_rel);
}			CustomPathMethods;

/*
 * 自定义扫描。同样事不多:需能生成与扫描对应的
 * ScanState。
 */
typedef struct CustomScanMethods
{
	const char *CustomName;

	/* 从 CustomScan 计划节点创建执行状态 (CustomScanState) */
	Node	   *(*CreateCustomScanState) (CustomScan *cscan);
} CustomScanMethods;

/*
 * CustomScanState 的执行期方法。比自定义路径或扫描
 * 所需更复杂。
 */
typedef struct CustomExecMethods
{
	const char *CustomName;

	/* 必需的执行器方法 */
	void		(*BeginCustomScan) (CustomScanState *node,
									EState *estate,
									int eflags);
	TupleTableSlot *(*ExecCustomScan) (CustomScanState *node);
	void		(*EndCustomScan) (CustomScanState *node);
	void		(*ReScanCustomScan) (CustomScanState *node);

	/* 可选方法:支持 mark/restore 时需要 */
	void		(*MarkPosCustomScan) (CustomScanState *node);
	void		(*RestrPosCustomScan) (CustomScanState *node);

	/* 可选方法:支持并行执行时需要 */
	Size		(*EstimateDSMCustomScan) (CustomScanState *node,
										  ParallelContext *pcxt);
	void		(*InitializeDSMCustomScan) (CustomScanState *node,
											ParallelContext *pcxt,
											void *coordinate);
	void		(*ReInitializeDSMCustomScan) (CustomScanState *node,
											  ParallelContext *pcxt,
											  void *coordinate);
	void		(*InitializeWorkerCustomScan) (CustomScanState *node,
											   shm_toc *toc,
											   void *coordinate);
	void		(*ShutdownCustomScan) (CustomScanState *node);

	/* 可选:在 EXPLAIN 中打印附加信息 */
	void		(*ExplainCustomScan) (CustomScanState *node,
									  List *ancestors,
									  ExplainState *es);
} CustomExecMethods;

extern void RegisterCustomScanMethods(const CustomScanMethods *methods);
extern const CustomScanMethods *GetCustomScanMethods(const char *CustomName,
													 bool missing_ok);

#endif							/* EXTENSIBLE_H */
