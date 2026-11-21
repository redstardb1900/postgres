/*-------------------------------------------------------------------------
 *
 * dest.h
 *	  通信目的地的支持
 *
 * 每当后端执行返回元组的查询，结果须送到某处。
 * 例如：
 *
 *	  - 跑独立后端（无 postmaster）并把结果
 *		返回给交互用户时，stdout 才是目的地。
 *	。
 *
 *	  - 带前端跑后端且前端执行
 *		PQexec()/PQfn() 时，远程进程是目的地。
 *		结果经 backend/libpq 中函数发到前端。
 *	。
 *
 *	  - 系统内部执行查询时目的地为 DestNone。
 *		结果被丢弃。
 *
 * dest.c 定义三个实现目的地管理的函数：
 *
 * BeginCommand：命令开始时初始化目的地。
 * CreateDestReceiver：返回指向目的地专用接收函数
 * 结构的指针。
 * EndCommand：命令结束时清理目的地。
 *
 * BeginCommand/EndCommand 对每条收到的 SQL 执行一次。
 *
 * CreateDestReceiver 返回适于指定目的地的接收对象。
 * 执行器以及可返回元组的实用语句会收到该
 * DestReceiver* 指针。每次执行器运行或实用语句执行
 * 调用 rStartup，再反复 receiveSlot（零或多次），
 * 然后 rShutdown。同一接收对象可多次重用；最终调用
 * rDestroy 销毁。
 * 。
 *
 * 有时接收对象需要额外参数，须在调用
 * CreateDestReceiver 后再传入。因不同类型参数集不同，
 * 本模块不处理，而由调用代码直接调用接收类型
 * 专用函数。
 * 。
 *
 * CreateDestReceiver 返回的 DestReceiver 可能是静态分配
 * 对象（无需局部状态的目的地类型），此时 rDestroy
 * 为空操作。也可能是 palloc 对象，首字段为 DestReceiver
 * 并含额外字段（例见 printtup.c）。接收函数可通过
 * 强制转换传入的 DestReceiver* 访问这些字段。palloc
 * 对象由 rDestroy 做 pfree。注意 CreateDestReceiver 的
 * 调用者应在足够长寿的内存上下文中调用，以免对象
 * 仍需要时已消失。
 * 。
 * 。
 *
 * 特殊规定：None_Receiver 是 DestNone 目的地永久可用的
 * 接收对象，以免门户/游标操作中无用的创建/销毁。
 * 。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/dest.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEST_H
#define DEST_H

#include "executor/tuptable.h"
#include "tcop/cmdtag.h"




/* ----------------
 *		CommandDest 是标识期望目的地的简单手段。
 *		将来多半需要改进。
 *
 * 注意：全局变量 whereToSendOutput 仅允许 DestNone、
 * DestDebug、DestRemote。其他值可用作个别命令的
 * 目的地。
 * ----------------
 */
typedef enum
{
	DestNone,					/* 结果被丢弃 */
	DestDebug,					/* 结果到调试输出 */
	DestRemote,					/* 结果发到前端进程 */
	DestRemoteExecute,			/* 发到前端，在 Execute 命令中 */
	DestRemoteSimple,			/* 发到前端，无系统目录访问 */
	DestSPI,					/* 结果发到 SPI 管理器 */
	DestTuplestore,				/* 结果发到 Tuplestore */
	DestIntoRel,				/* 结果发到关系（SELECT INTO） */
	DestCopyOut,				/* 结果发到 COPY TO 代码 */
	DestSQLFunction,			/* 结果发到 SQL 语言函管 */
	DestTransientRel,			/* 结果发到瞬态关系 */
	DestTupleQueue				/* 结果发到元组队列 */
} CommandDest;

/* ----------------
 *		DestReceiver 是目的地专用局部状态的基类型。
 *		最简单情形无状态信息，只有执行器须调用的
 *		函数指针。
 *
 * 注意：receiveSlot 须传入含与 rStartup 相同 TupleDesc
 * 的槽。返回 bool："true" 表示继续处理，"false" 表示
 * 提早停止，如同已到扫描末尾。
 * 。
 * ----------------
 */
typedef struct _DestReceiver DestReceiver;

struct _DestReceiver
{
	/* 对每个待输出元组调用： */
	bool		(*receiveSlot) (TupleTableSlot *slot,
								DestReceiver *self);
	/* 每次执行器运行的初始化与关闭： */
	void		(*rStartup) (DestReceiver *self,
							 int operation,
							 TupleDesc typeinfo);
	void		(*rShutdown) (DestReceiver *self);
	/* 销毁接收对象自身（若动态分配） */
	void		(*rDestroy) (DestReceiver *self);
	/* 本接收器的 CommandDest 码 */
	CommandDest mydest;
	/* 私有字段可能出现在此点之后…… */
};

extern PGDLLIMPORT DestReceiver *None_Receiver; /* DestNone 的永久
												 * 接收 */

/* 主要的目的地管理函数 */

extern void BeginCommand(CommandTag commandTag, CommandDest dest);
extern DestReceiver *CreateDestReceiver(CommandDest dest);
extern void EndCommand(const QueryCompletion *qc, CommandDest dest,
					   bool force_undecorated_output);
extern void EndReplicationCommand(const char *commandTag);

/* 与目的地管理相关的附加函数 */

extern void NullCommand(CommandDest dest);
extern void ReadyForQuery(CommandDest dest);

#endif							/* DEST_H */
