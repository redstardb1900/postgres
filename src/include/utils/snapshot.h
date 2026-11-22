/*-------------------------------------------------------------------------
 *
 * snapshot.h
 *	  POSTGRES 快照定义
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "access/htup.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "lib/pairingheap.h"
#include "storage/buf.h"


/*
 * 不同的快照类型。我们用 SnapshotData 结构表示
 * "常规"（MVCC）快照与语义不同的"特殊"快照。具
 * 体语义由其类型编码。
 *
 * 各类型行为应与枚举值一并文档化，最好用不限于某
 * 一表 AM 的措辞。
 *
 * 用快照类型而非昔日回调，是为让同一快照可用于
 * 不同表 AM，而无需每 AM 一个回调。
 * 见上。
 */
typedef enum SnapshotType
{
	/*-------------------------------------------------------------------------
	 * 当且仅当元组对该 MVCC 快照有效时可见。
	 *
	 * 此处考虑以下影响：
	 * - 截至给定快照时刻已提交的全部事务
	 * - 本事务的先前命令
	 *
	 * Does _not_ include:
	 * - 快照显示为进行中的事务
	 * - 快照取得之后启动的事务
	 * - 当前命令所做更改
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_MVCC = 0,

	/*-------------------------------------------------------------------------
	 * 当且仅当元组"对其自身"有效时可见。
	 *
	 * 此处考虑以下影响：
	 * - 全部已提交事务（截至当前时刻）
	 * - 本事务的先前命令
	 * - 当前命令所做更改
	 *
	 * Does _not_ include:
	 * - 进行中的事务（截至当前时刻）
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_SELF,

	/*
	 * 任何元组可见。
	 */
	SNAPSHOT_ANY,

	/*
	 * 当且仅当元组作为 TOAST 行有效时可见。
	 */
	SNAPSHOT_TOAST,

	/*-------------------------------------------------------------------------
	 * 当且仅当元组有效且含打开事务影响时可见。
	 * 见上。
	 *
	 * 此处考虑以下影响：
	 * - 全部已提交与进行中的事务（截至当前时刻）
	 * - 本事务的先前命令
	 * - 当前命令所做更改
	 *
	 * 就当前事务与已提交/中止事务的影响而言，本质
	 * 上像 SNAPSHOT_SELF。但它还包括其他仍在进行中
	 * 的事务的影响。
	 *
	 * 特殊技巧：用此类快照判定可见性时，传入的
	 * 快照结构用作输出参数，返回影响该元组的并发
	 * 事务 xid。若元组 xmin 是另一仍进行中的事务，则
	 * snapshot->xmin 设为该 xmin；若已提交有效、已提交
	 * 死/本事务则 InvalidTransactionId。
	 * snapshot->xmax 与元组 xmax 同理。若元组为投机
	 * 插入（插入者可能不中止整事务就撤回插入），关
	 * 联 token 也经 snapshot->speculativeToken 返回。
	 * 另见 InitDirtySnapshot()。
	 * 见上。
	 * 见上。
	 * -------------------------------------------------------------------------
	 */
	SNAPSHOT_DIRTY,

	/*
	 * 当且仅当遵循 SNAPSHOT_MVCC 规则时可见，但支持
	 * 在时间旅行上下文调用（用于逻辑解码中解码
	 * 目录内容）。
	 */
	SNAPSHOT_HISTORIC_MVCC,

	/*
	 * 当且仅当元组可能对某事务可见时为真；若肯定对
	 * 人人已死即可 vacuum，则为假。
	 *
	 * 可见性检查须用要使用的 xmin 地平线设好
	 * min。
	 */
	SNAPSHOT_NON_VACUUMABLE
} SnapshotType;

typedef struct SnapshotData *Snapshot;

#define InvalidSnapshot		((Snapshot) NULL)

/*
 * 表示各类可能快照的结构。
 *
 * 有几种不同的快照：
 * * 普通 MVCC 快照
 * * 恢复期间取得的 MVCC 快照（热备模式）
 * * 逻辑解码所用的历史 MVCC 快照
 * * 传给 HeapTupleSatisfiesDirty() 的快照
 * * 传给 HeapTupleSatisfiesNonVacuumable() 的快照
 * * 用于 SatisfiesAny、Toast、Self 且不访问成员的
 *	 快照。
 *
 * TODO：最好像解析器与执行器节点那样用 NodeTag
 * 拆分此结构，每类快照一种类型，以免各字段含义
 * 过载。
 * 见上。
 */
typedef struct SnapshotData
{
	SnapshotType snapshot_type; /* 快照类型 */

	/*
	 * 其余字段仅用于 MVCC 快照，特殊快照中通常为零。
	 * 但 xmin/xmax 由 HeapTupleSatisfiesDirty 特殊使
	 * 用，xmin 由 HeapTupleSatisfiesNonVacuumable 特殊
	 * 使用。
	 *
	 * MVCC 快照从不可见 XIDs >= xmax 的影响。可见除快
	 * 照中列出者外的全部更早 XIDs 的影响。存 xmin 是为
	 * 优化，以免多数元组需搜索 XID 数组。
	 * 见上。
	 */
	TransactionId xmin;			/* 凡 XID < xmin 对我可见 */
	TransactionId xmax;			/* 凡 XID >= xmax 对我不可见 */

	/*
	 * 对普通 MVCC 快照，含所有进行中的事务 ID；若在
	 * 恢复期间取得则为空。对历史 MVCC 快照含义相反，
	 * 即含 xmin 与 xmax 之间*已提交*的事务。
	 * 见上。
	 *
	 * 注意: xip[] 中 id 满足 xmin <= xip[i] < xmax
	 */
	TransactionId *xip;
	uint32		xcnt;			/* xip[] 事务 id 数 */

	/*
	 * 对非历史 MVCC 快照，含进行中的子事务 ID（恢复
	 * 期间取得时还有其他进行中事务）。对历史快照含
	 * 赋给被重放事务的*全部* xid，含顶层 xid。
	 * 见上。
	 *
	 * 注意：subxip[] 中所有 id >= xmin，但我们不费心
	 * 滤掉 >= xmax 者
	 */
	TransactionId *subxip;
	int32		subxcnt;		/* subxip[] 事务 id 数 */
	bool		suboverflowed;	/* subxip 数组是否已溢出？ */

	bool		takenDuringRecovery;	/* 恢复形态的快照？ */
	bool		copied;			/* 若为静态快照则为假 */

	CommandId	curcid;			/* 本事务中 CID < curcid 可见 */

	/*
	 * HeapTupleSatisfiesDirty 的额外返回值，MVCC
	 * 不用。
	 */
	uint32		speculativeToken;

	/*
	 * 对 SNAPSHOT_NON_VACUUMABLE（希望将来还有更多）
	 * 用于判定行能否被 vacuum。
	 */
	struct GlobalVisState *vistest;

	/*
	 * 簿记信息，由快照管理器使用
	 */
	uint32		active_count;	/* ActiveSnapshot 栈引用计数 */
	uint32		regd_count;		/* RegisteredSnapshots 引用数 */
	pairingheap_node ph_node;	/* RegisteredSnapshots 堆中的链接 */

	TimestampTz whenTaken;		/* 取得快照的时间戳 */
	XLogRecPtr	lsn;			/* 取得时在 WAL 流中的位置 */

	/*
	 * GetSnapshotData() 构建本快照时的事务完成计数。
	 * 可在自上次 GetSnapshotData() 以来无事务完成时
	 * 避免重算静态快照。
	 */
	uint64		snapXactCompletionCount;
} SnapshotData;

#endif							/* SNAPSHOT_H */
