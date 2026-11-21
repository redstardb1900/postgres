/*-------------------------------------------------------------------------
 *
 * predicate_internals.h
 *	  POSTGRES 内部谓词加锁定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/predicate_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREDICATE_INTERNALS_H
#define PREDICATE_INTERNALS_H

#include "lib/ilist.h"
#include "storage/lock.h"
#include "storage/lwlock.h"

/*
 * 提交号。
 */
typedef uint64 SerCommitSeqNo;

/*
 * 保留的提交序号：
 *	- 0 保留表示不存在的 SLRU 项；它无法
 *	  用作 SerCommitSeqNo，即使是无效值
 *	- InvalidSerCommitSeqNo 表示事务
 *	  尚未提交，故用大于全部有效序号的
 *	  值，使比较行为符合预期
 *	- RecoverySerCommitSeqNo 用于指崩溃/
 *	  恢复前发生的事务，因我们在该点重启
 *	  序号。它早于全部正常序号，
 *	  且仅用于已恢复的预备事务
 */
#define InvalidSerCommitSeqNo		((SerCommitSeqNo) PG_UINT64_MAX)
#define RecoverySerCommitSeqNo		((SerCommitSeqNo) 1)
#define FirstNormalSerCommitSeqNo	((SerCommitSeqNo) 2)

/*
 * SERIALIZABLEXACT 结构保存每个可串行化
 * 数据库事务为支持 SSI 技术所需的信息。
 *
 * 共享内存中用自制链表管理这些项。
 * 可串行化事务取得快照时使用一项。
 * 除非事务回滚，该项通常必须保留到
 * 全部并发事务完成。（对 READ ONLY 事务
 * 有特殊优化，常允许更早清理。）
 * 回滚的事务会尽快
 * 清理。
 *
 * 已提交事务的清理资格通常由比较该事务的
 * finishedBefore 字段与
 * SxactGlobalXmin.
 */
typedef struct SERIALIZABLEXACT
{
	VirtualTransactionId vxid;	/* 执行进程总有其中之
								 * 一。 */

	/*
	 * 用两个数跟踪事务提交次序。提交前
	 * 事务标为预备，并设置 prepareSeqNo。
	 * 提交后不久标为已提交，并设置 commitSeqNo。
	 * 这不给出严格提交序，但两值合用对我们
	 * 已够用：若无法确定两次提交的精确
	 * 次序，总可偏安全地假定存在冲突。
	 * （见上行。）
	 *
	 * 注意：即使未用两阶段提交，事务在提交处理
	 * 期间也会短暂标为预备。但用两阶段提交时，
	 * 事务可在预备态停留一些时间。
	 * 。
	 */
	SerCommitSeqNo prepareSeqNo;
	SerCommitSeqNo commitSeqNo;

	/* 两值不会同时有意义 */
	union
	{
		SerCommitSeqNo earliestOutConflictCommit;	/* 已提交且有出
													 * 冲突时 */
		SerCommitSeqNo lastCommitBeforeSnapshot;	/* 未提交或无出
													 * 冲突时 */
	}			SeqNo;
	dlist_head	outConflicts;	/* 我们无法读取其数据的写
								 * 事务表。 */
	dlist_head	inConflicts;	/* 无法看到我们写出的读
								 * 事务表。 */
	dlist_head	predicateLocks; /* 关联 PREDICATELOCK 对象的列表 */
	dlist_node	finishedLink;	/* 链表节于
								 * FinishedSerializableTransactions */
	dlist_node	xactLink;		/* PredXact->activeList/availableList */

	/*
	 * perXactPredicateListLock 仅用于并行查询：它保护
	 * 本 SERIALIZABLEXACT 谓词锁列表防同会话
	 * 工作进程。
	 */
	LWLock		perXactPredicateListLock;

	/*
	 * 对只读事务：可能与之冲突的并发读写事务列表；
	 * 对读写事务则反之
	 */
	dlist_head	possibleUnsafeConflicts;

	TransactionId topXid;		/* 事务顶层 xid（若有）；
								 * 否则无效 */
	TransactionId finishedBefore;	/* 无效=仍在运行；否则当无
									 * 可串行化 xid 早于该值时
									 * 结构过期。 */
	TransactionId xmin;			/* 该事务快照的 xmin */
	uint32		flags;			/* 下文定义值的按位或组合 */
	int			pid;			/* 关联进程的 pid */
	int			pgprocno;		/* 关联进程的 pgprocno */
} SERIALIZABLEXACT;

#define SXACT_FLAG_COMMITTED			0x00000001	/* 已提交 */
#define SXACT_FLAG_PREPARED				0x00000002	/* 即将提交 */
#define SXACT_FLAG_ROLLED_BACK			0x00000004	/* 已回滚 */
#define SXACT_FLAG_DOOMED				0x00000008	/* 将回滚 */
/*
 * 下列标志实际表示被标事务存在指向
 * *先于它提交的事务* 的出冲突。很难把
 * 这放进合理长度的名字。
 */
#define SXACT_FLAG_CONFLICT_OUT			0x00000010
#define SXACT_FLAG_READ_ONLY			0x00000020
#define SXACT_FLAG_DEFERRABLE_WAITING	0x00000040
#define SXACT_FLAG_RO_SAFE				0x00000080
#define SXACT_FLAG_RO_UNSAFE			0x00000100
#define SXACT_FLAG_SUMMARY_CONFLICT_IN	0x00000200
#define SXACT_FLAG_SUMMARY_CONFLICT_OUT 0x00000400
/*
 * 下列标志表示事务已部分释放，但仍
 * 保留，因并行工作进程可能仍引用它。
 * 事务结束时由领导者回收。
 */
#define SXACT_FLAG_PARTIALLY_RELEASED	0x00000800

typedef struct PredXactListData
{
	dlist_head	availableList;
	dlist_head	activeList;

	/*
	 * 注册与清理可串行化事务时维护这些全局变量。
	 * 它们必须对全部后端全局可见，但
	 * predicate.c 外不需要。受下列锁保护：
	 * SerializableXactHashLock.
	 */
	TransactionId SxactGlobalXmin;	/* 活跃可串行化事务的
									 * 全局 xmin */
	int			SxactGlobalXminCount;	/* 有多少活跃可串行化
										 * 事务持有此 xmin */
	int			WritableSxactCount; /* 有多少非只读可串行化
									 * 事务处于活跃 */
	SerCommitSeqNo LastSxactCommitSeqNo;	/* 可串行化事务提交
											 * 的严格单调递增
											 * 序号 */
	/* 由 SerializableXactHashLock 保护。 */
	SerCommitSeqNo CanPartialClearThrough;	/* 可清理谓词锁与
											 * 已交事务 inConflicts
											 * 直至该序号
											 *  */
	/* 由 SerializableFinishedListLock 保护。 */
	SerCommitSeqNo HavePartialClearedThrough;	/* 已清理到此
												 * 序号 */
	SERIALIZABLEXACT *OldCommittedSxact;	/* 哑 sxact 的共享副本 */

	SERIALIZABLEXACT *element;
}			PredXactListData;

typedef struct PredXactListData *PredXactList;

#define PredXactListDataSize \
		((Size)MAXALIGN(sizeof(PredXactListData)))


/*
 * 下列类型用于提供事务对之间的读写冲突
 * 列表。因所需信息完全相同，也用于记录
 * 可能不安全的事务关系，以便为只读事务
 * 识别安全快照。
 *
 * RWConflictData 未用于记录任一类关系时，
 * 放在"可用"列表上。outLink 字段用于
 * 维护该列表。
 */
typedef struct RWConflictData
{
	dlist_node	outLink;		/* 自 sxact 出冲突列表的链节 */
	dlist_node	inLink;			/* 入 sxact 冲突列表的链节 */
	SERIALIZABLEXACT *sxactOut;
	SERIALIZABLEXACT *sxactIn;
} RWConflictData;

typedef struct RWConflictData *RWConflict;

#define RWConflictDataSize \
		((Size)MAXALIGN(sizeof(RWConflictData)))

typedef struct RWConflictPoolHeaderData
{
	dlist_head	availableList;
	RWConflict	element;
}			RWConflictPoolHeaderData;

typedef struct RWConflictPoolHeaderData *RWConflictPoolHeader;

#define RWConflictPoolHeaderDataSize \
		((Size)MAXALIGN(sizeof(RWConflictPoolHeaderData)))


/*
 * SERIALIZABLEXIDTAG 标识赋给可串行化事务
 * 或其任一子事务的 xid。
 */
typedef struct SERIALIZABLEXIDTAG
{
	TransactionId xid;
} SERIALIZABLEXIDTAG;

/*
 * SERIALIZABLEXID 从可串行化事务的
 * TransactionId 链到相关 SERIALIZABLEXACT 记录，
 * 即使事务已完成且连接已关闭。
 *
 * 参与谓词加锁的事务首次被赋新顶层
 * 事务 ID 时创建。若事务从不写任何东西，
 * 可能永不发生。它们随相关可串行化
 * 事务对象一并移除。
 *
 * 必要时用 SubTransGetTopmostTransaction 从可能
 * 来自子事务的 XID 取得顶层 XID。
 */
typedef struct SERIALIZABLEXID
{
	/* hash key */
	SERIALIZABLEXIDTAG tag;

	/* data */
	SERIALIZABLEXACT *myXact;	/* 指向顶层事务数据的指针 */
} SERIALIZABLEXID;


/*
 * PREDICATELOCKTARGETTAG 标识可作为谓词锁
 * 目标的数据库对象。
 *
 * 注意：所用散列函数未正确尊重标签长度——
 * 若结构长度不是四字节的倍数，它会越出标签
 * 末尾到四字节边界。若修改本结构，请确保
 * 任何松弛空间已初始化，以免中间或末尾的
 * 随机字节被纳入散列。
 *
 * TODO SSI：若总用相同字段表示同类值，应
 * 重命名它们。待确认无例外后再做。
 * 因索引是带块与元组的关系，重命名看来
 * 很可能可行。若否，或需拆分最后字段，
 * 用一部分作目标类型，以便知道如何解释
 * 数据
 */
typedef struct PREDICATELOCKTARGETTAG
{
	uint32		locktag_field1; /* 32 位 ID 字段 */
	uint32		locktag_field2; /* 32 位 ID 字段 */
	uint32		locktag_field3; /* 32 位 ID 字段 */
	uint32		locktag_field4; /* 32 位 ID 字段 */
} PREDICATELOCKTARGETTAG;

/*
 * PREDICATELOCKTARGET 表示其上存在谓词锁的
 * 数据库对象。
 *
 * 共享内存中维护这些对象的散列链表。当对
 * 尚无项的对象请求谓词锁时加入一项。当其
 * 列表上最后一把锁移除时删除该项。
 * 。
 */
typedef struct PREDICATELOCKTARGET
{
	/* hash key */
	PREDICATELOCKTARGETTAG tag; /* 可加锁对象的唯一标识 */

	/* data */
	dlist_head	predicateLocks; /* 关联谓词锁目标的
								 * PREDICATELOCK 列表 */
} PREDICATELOCKTARGET;


/*
 * PREDICATELOCKTAG 标识一把个别谓词锁。
 *
 * 它是谓词锁目标（可加锁对象）与已在该
 * 目标上取得锁的可串行化事务之组合。
 * 。
 */
typedef struct PREDICATELOCKTAG
{
	PREDICATELOCKTARGET *myTarget;
	SERIALIZABLEXACT *myXact;
} PREDICATELOCKTAG;

/*
 * PREDICATELOCK 表示一把个别锁。
 *
 * 可读相关数据库对象时在此创建项，或由多个
 * 更细粒度目标提升而来。与某可串行化事务
 * 相关的全部项在该可串行化事务清理时移除。
 * 合并为单个更粗粒度锁项时也可移除。
 * 。
 */
typedef struct PREDICATELOCK
{
	/* hash key */
	PREDICATELOCKTAG tag;		/* 锁的唯一标识 */

	/* data */
	dlist_node	targetLink;		/* PREDICATELOCKTARGET 之
								 * 列表链节 */
	dlist_node	xactLink;		/* SERIALIZABLEXACT 之
								 * 列表链节 */
	SerCommitSeqNo commitSeqNo; /* 仅用于汇总谓词锁 */
} PREDICATELOCK;


/*
 * LOCALPREDICATELOCK 表示一份本地副本，数据也
 * 存在于 PREDICATELOCK 表中，为无需取得 LWLock
 * 即可快速访问而组织。纯为优化。
 *
 * 每个可串行化事务创建自己的本地散列表保存
 * 这些项的集合。用于判定何时应将若干细粒度
 * 锁提升为单个更粗粒度锁。信息与
 * PREDICATELOCK 数据大致并行维护，但因本数据
 * 不受锁保护且仅用于优化启发式，在少数维护
 * 精确数据代价高昂的边角情形允许漂移。
 * 。
 *
 * 散列表在可串行化事务取得快照时创建，
 * 事务完成时释放其内存。
 */
typedef struct LOCALPREDICATELOCK
{
	/* hash key */
	PREDICATELOCKTARGETTAG tag; /* 可加锁对象的唯一标识 */

	/* data */
	bool		held;			/* 锁自持，还是仅其子？	*/
	int			childLocks;		/* 当前所持子锁数 */
} LOCALPREDICATELOCK;


/*
 * 可取得的谓词锁类型。
 */
typedef enum PredicateLockTargetType
{
	PREDLOCKTAG_RELATION,
	PREDLOCKTAG_PAGE,
	PREDLOCKTAG_TUPLE
	/* TODO SSI：索引加锁或需其他类型 */
} PredicateLockTargetType;


/*
 * 本结构用于快速捕获全部谓词锁的副本。
 * 当前仅由 pg_lock_status 函数使用，
 * 后者又被 pg_locks 视图使用。
 */
typedef struct PredicateLockData
{
	int			nelements;
	PREDICATELOCKTARGETTAG *locktags;
	SERIALIZABLEXACT *xacts;
} PredicateLockData;


/*
 * 这些宏定义如何把可加锁对象的逻辑 ID 映射到
 * PREDICATELOCKTARGETTAG 的物理字段。请用它们设值，
 * 勿直接访问字段。注意 target 会被多次求值！
 */
#define SET_PREDICATELOCKTARGETTAG_RELATION(locktag,dboid,reloid) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = InvalidBlockNumber, \
	 (locktag).locktag_field4 = InvalidOffsetNumber)

#define SET_PREDICATELOCKTARGETTAG_PAGE(locktag,dboid,reloid,blocknum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = InvalidOffsetNumber)

#define SET_PREDICATELOCKTARGETTAG_TUPLE(locktag,dboid,reloid,blocknum,offnum) \
	((locktag).locktag_field1 = (dboid), \
	 (locktag).locktag_field2 = (reloid), \
	 (locktag).locktag_field3 = (blocknum), \
	 (locktag).locktag_field4 = (offnum))

#define GET_PREDICATELOCKTARGETTAG_DB(locktag) \
	((Oid) (locktag).locktag_field1)
#define GET_PREDICATELOCKTARGETTAG_RELATION(locktag) \
	((Oid) (locktag).locktag_field2)
#define GET_PREDICATELOCKTARGETTAG_PAGE(locktag) \
	((BlockNumber) (locktag).locktag_field3)
#define GET_PREDICATELOCKTARGETTAG_OFFSET(locktag) \
	((OffsetNumber) (locktag).locktag_field4)
#define GET_PREDICATELOCKTARGETTAG_TYPE(locktag)							 \
	(((locktag).locktag_field4 != InvalidOffsetNumber) ? PREDLOCKTAG_TUPLE : \
	 (((locktag).locktag_field3 != InvalidBlockNumber) ? PREDLOCKTAG_PAGE :   \
	  PREDLOCKTAG_RELATION))

/*
 * 两阶段提交状态文件记录。有两类：对每个
 * 事务生成一条每事务记录，以及可变数量的
 * 每谓词锁记录。
 */
typedef enum TwoPhasePredicateRecordType
{
	TWOPHASEPREDICATERECORD_XACT,
	TWOPHASEPREDICATERECORD_LOCK
} TwoPhasePredicateRecordType;

/*
 * 用于重建 SERIALIZABLEXACT 的每事务信息。对
 * 已恢复的预备事务，多数字段无意义，故
 * 所需不多。
 *
 * 尤其不记录预备事务的出入冲突列表，因
 * 关联 SERIALIZABLEXACTs 在恢复后不可用。
 * 改为通过设置事务的汇总冲突入/出标志，
 * 仅记录每类冲突是否存在。
 * 。
 */
typedef struct TwoPhasePredicateXactRecord
{
	TransactionId xmin;
	uint32		flags;
} TwoPhasePredicateXactRecord;

/* 每锁状态 */
typedef struct TwoPhasePredicateLockRecord
{
	PREDICATELOCKTARGETTAG target;
	uint32		filler;			/* 避免回溯补丁修复中长度变化 */
} TwoPhasePredicateLockRecord;

typedef struct TwoPhasePredicateRecord
{
	TwoPhasePredicateRecordType type;
	union
	{
		TwoPhasePredicateXactRecord xactRecord;
		TwoPhasePredicateLockRecord lockRecord;
	}			data;
} TwoPhasePredicateRecord;

/*
 * 定义表示"空" SERIALIZABLEXACT 引用的宏。
 */
#define InvalidSerializableXact ((SERIALIZABLEXACT *) NULL)


/*
 * 需要了解谓词加锁内部细节的
 * 函数定义。
 */
extern PredicateLockData *GetPredicateLockStatusData(void);
extern int	GetSafeSnapshotBlockingPids(int blocked_pid,
										int *output, int output_size);

#endif							/* PREDICATE_INTERNALS_H */
