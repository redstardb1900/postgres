/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES 共享缓存失效通信定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/sinval.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include <signal.h>

#include "storage/relfilelocator.h"

/*
 * 我们支持若干种共享失效消息：
 *	* 使某 catcache 中某元组失效
 *	* 使某系统目录的全部 catcache 条目失效
 *	* 使某逻辑关系的 relcache 条目失效
 *	* 使全部 relcache 条目失效
 *	* 使某物理关系的 smgr 缓存条目失效
 *	* 使某数据库的映射关系映射失效
 *	* 使可能用于扫描某关系的任何已保存快照失效
 * 需要时可再增类型。消息类型由消息结构的
 * 首个 "int8" 字段标识。零或正值表示
 * 特定-catcache 失效消息（兼作 catcache ID 字段）。
 * 负值标识其他消息类型，见下方代码。
 *
 * Catcache 失效事件最初由检测系统目录中的元组插入、
 * 更新与删除驱动（见 CacheInvalidateHeapTuple）。
 * 一次更新可产生两个失效事件，分别针对旧元组与新
 * 元组，但若元组哈希键未变则合并为一事件。
 * 注意失效事件本身并不说明元组是在插入还是删除。
 * 另因我们只传哈希键，存在因碰巧
 * 哈希键相同而发生不必要失效的小
 * 风险。
 *
 * 注意有些系统目录上有多个缓存（索引不同）。
 * 在此类目录中检测到元组失效时，必须为其每个
 * 缓存分别生成 catcache 失效消息，因为
 * 哈希键一般不同。
 *
 * Catcache、relcache 与快照失效是事务性的，故
 * 在提交时发给其他后端。在产生方后端内部，
 * 它们也在 CommandCounterIncrement 时处理，使同事务中
 * 后续命令看到新状态。产生方后端在中止时也须
 * 处理它们，以冲掉已从不再有效的条目加载的任何
 * 缓存状态。
 *
 * smgr 与关系映射失效是非事务性的：在底层文件
 * 变更做出时立即发送。
 */

typedef struct
{
	int8		id;				/* 缓存 ID 须为首 */
	Oid			dbId;			/* 数据库 ID，共享关系则为 0 */
	uint32		hashValue;		/* 本 catcache 键的哈希值 */
} SharedInvalCatcacheMsg;

#define SHAREDINVALCATALOG_ID	(-1)

typedef struct
{
	int8		id;				/* 类型字段 须为首 */
	Oid			dbId;			/* 数据库 ID，共享目录则为 0 */
	Oid			catId;			/* 内容已无效的目录 ID */
} SharedInvalCatalogMsg;

#define SHAREDINVALRELCACHE_ID	(-2)

typedef struct
{
	int8		id;				/* 类型字段 须为首 */
	Oid			dbId;			/* 数据库 ID，共享关系则为 0 */
	Oid			relId;			/* 关系 ID，整表 relcache 为 0 */
} SharedInvalRelcacheMsg;

#define SHAREDINVALSMGR_ID		(-3)

typedef struct
{
	/* 注意：字段布局选为装入 16 字节 */
	int8		id;				/* 类型字段 须为首 */
	int8		backend_hi;		/* 临时关系时后端 ID 高位 */
	uint16		backend_lo;		/* 临时关系时后端 ID 低位 */
	RelFileLocator rlocator;	/* spcOid/dbOid/relNumber */
} SharedInvalSmgrMsg;

#define SHAREDINVALRELMAP_ID	(-4)

typedef struct
{
	int8		id;				/* 类型字段 须为首 */
	Oid			dbId;			/* 数据库 ID，共享目录则为 0 */
} SharedInvalRelmapMsg;

#define SHAREDINVALSNAPSHOT_ID	(-5)

typedef struct
{
	int8		id;				/* 类型字段 须为首 */
	Oid			dbId;			/* 数据库 ID，共享关系则为 0 */
	Oid			relId;			/* 关系 ID */
} SharedInvalSnapshotMsg;

typedef union
{
	int8		id;				/* 类型字段 须为首 */
	SharedInvalCatcacheMsg cc;
	SharedInvalCatalogMsg cat;
	SharedInvalRelcacheMsg rc;
	SharedInvalSmgrMsg sm;
	SharedInvalRelmapMsg rm;
	SharedInvalSnapshotMsg sn;
} SharedInvalidationMessage;


/* 已处理消息计数；不必担心溢出。 */
extern PGDLLIMPORT uint64 SharedInvalidMessageCounter;

extern PGDLLIMPORT volatile sig_atomic_t catchupInterruptPending;

extern void SendSharedInvalidMessages(const SharedInvalidationMessage *msgs,
									  int n);
extern void ReceiveSharedInvalidMessages(void (*invalFunction) (SharedInvalidationMessage *msg),
										 void (*resetFunction) (void));

/* 追赶事件信号处理（PROCSIG_CATCHUP_INTERRUPT） */
extern void HandleCatchupInterrupt(void);

/*
 * 直接从信号处理函数启用/禁用追赶事件处理。
 * 启用例程先处理自上次禁用以来已发生的任何
 * 追赶事件。
 */
extern void ProcessCatchupInterrupt(void);

extern int	xactGetCommittedInvalidationMessages(SharedInvalidationMessage **msgs,
												 bool *RelcacheInitFileInval);
extern void ProcessCommittedInvalidationMessages(SharedInvalidationMessage *msgs,
												 int nmsgs, bool RelcacheInitFileInval,
												 Oid dbid, Oid tsid);

extern void LocalExecuteInvalidationMessage(SharedInvalidationMessage *msg);

#endif							/* SINVAL_H */
