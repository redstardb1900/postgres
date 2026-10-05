/*-------------------------------------------------------------------------
 *
 * predicate.c
 *	  POSTGRES 谓词加锁
 *	  用以支持完整的可串行化事务隔离
 *
 *
 * 所采取的做法是实现可串行化快照隔离（SSI），
 * 其最初描述见于这篇论文：
 *
 *	Michael J. Cahill, Uwe Röhm, and Alan D. Fekete. 2008.
 *	Serializable isolation for snapshot databases.
 *	In SIGMOD '08: Proceedings of the 2008 ACM SIGMOD
 *	international conference on Management of data,
 *	pages 729-738, New York, NY, USA. ACM.
 *	http://doi.acm.org/10.1145/1376616.1376690
 *
 * and further elaborated in Cahill's doctoral thesis:
 *
 *	Michael James Cahill. 2009.
 *	Serializable Isolation for Snapshot Databases.
 *	Sydney Digital Theses.
 *	University of Sydney, School of Information Technologies.
 *	http://hdl.handle.net/2123/5353
 *
 *
 * 可串行化快照隔离（SSI）的谓词锁是 SIREAD
 * 锁，它们与普通锁差别很大，因此需要一套不同的
 * 结构来处理。需要用它们来检测
 * 读发生在写之前时的读写依赖（rw-conflict）。（当写
 * 先发生时，读事务可以通过
 * 检查 MVCC 数据来发现冲突。）
 *
 * (1)	除实际读到的元组外，它们还必须覆盖基于谓词
 *		本会被读到的元组范围。这
 *		需要通过对数据库
 *		对象（如页、索引范围或整张表）加锁来建模谓词。
 *
 * (2)	它们必须放在 RAM 中以便快速访问。因此，
 *		并不总能维持元组级粒度——当
 *		为存放它们而分配的空间接近耗尽时，一次
 *		加锁请求可能需要扫描这样的情形：单个
 *		事务持有许多细粒度锁，可以把它们合并
 *		成一把更粗粒度的锁。
 *
 * (3)	它们从不阻塞任何东西；就此而言它们更像标志而不是锁，
 *		尽管它们引用数据库对象，并被
 *		用来识别与普通写锁之间的读写依赖（rw-conflict）。
 *
 * (4)	它们虽与某个事务相关联，但必须在该事务
 *		成功 COMMIT 之后继续存在，直到所有
 *		重叠事务都完成。这甚至意味着它们
 *		必须在该事务的进程终止后仍然存活。不过，若
 *		顶层事务被回滚，会立即
 *		打上标记以便可以忽略，其 SIREAD 锁可以在
 *		那之后的任何时候释放。
 *
 * (5)	创建 SIREAD 锁或检查
 *		与它们的冲突的，只有可串行化事务。
 *
 * (6)	当发现顶层事务的写锁覆盖了
 *		同一事务已有的 SIREAD 锁时，该 SIREAD 锁
 *		可以删除。
 *
 * (7)	可串行化事务的写必须确保存在一条该事务的 xact
 *		记录，且寿命相同（直到
 *		所有并发事务完成，或该事务被
 *		回滚），以便指向该事务的读写依赖能够被
 *		检测到。
 *
 * 我们对只读事务使用一项优化。在某些
 * 情形下，可以证明只读事务的快照
 * 永远不会与其他事务冲突。这称为
 * "安全"快照（已知并非如此的则是"不安全"）。
 * 不过，在能够确定快照是否安全之前，必须等到
 * 所有并发的读/写事务都完成。
 *
 * 一旦已知只读事务拥有安全快照，它
 * 就可以释放其谓词锁，并免除进一步的
 * 谓词锁跟踪。READ ONLY DEFERRABLE 事务只
 * 在安全快照上运行，必要时等待出现一个可用的快照。
 *
 *
 * 用于管理对谓词加锁共享
 * 内存对象之访问的轻量锁必须按此顺序获取，并应按
 * 相反顺序释放：
 *
 *	SerializableFinishedListLock
 *		- 保护已完成、但由于与仍活动的事务重叠
 *			而可能仍然要紧的事务列表。
 *
 *	SerializablePredicateListLock
 *		- 保护某事务所持锁的链表。注意
 *			锁本身也由其各自锁目标的分区
 *			锁所覆盖；这把锁只影响
 *			连接与某事务相关的那些锁的链表。
 *		- 所有事务共享这一把锁（不做分区）。
 *		- 除正在运行某个活动事务的进程外，从不需要
 *			别的进程去遍历该事务所持有的锁列表，
 *			例外是共享领导事务的并行查询工作进程。
 *			并行情形下会额外取得一把按 sxact 的锁，
 *			见下文。
 *		- 另一进程需要为某事务
 *			修改其列表的情况相对少见，但确实会发生，例如
 *			带谓词锁的页发生索引页分裂，以及
 *			清理进程释放被谓词锁定的页。在此类情形下
 *			移除锁时，锁本身含有
 *			将其从列表中移除所需的指针。添加
 *			锁时，可以用事务结构中的锚点
 *			把锁加上。两者都不需要遍历列表。
 *		- 已终止事务的列表清理有时
 *			不是逐条进行的，此时不需要锁。
 *		- 由于上述原因，访问自己活动事务之
 *			列表的进程总是使用共享锁，无论它是在
 *			遍历还是维护该列表。这改善了
 *			常见访问模式的并发度。
 *		- 需要改动自己活动事务以外的
 *			某事务之列表的进程，必须取得排他
 *			锁。
 *
 *	SERIALIZABLEXACT 的成员 'perXactPredicateListLock'
 *		- 保护某事务所持谓词锁的链表。
 *			仅并行模式需要，此时多个后端共享
 *			同一个 SERIALIZABLEXACT 对象。若已排他持有
 *			SerializablePredicateListLock，则不需要。
 *
 *	PredicateLockHashPartitionLock(hashcode)
 *		- 同一把锁保护一个目标、该目标上的全部锁，以及
 *			该目标上锁的链表。
 *		- 需要不止一把时，按地址升序获取。
 *		- 需要全部时（罕见），按索引升序获取，使用
 *			PredicateLockHashPartitionLockByIndex(index).
 *
 *	SerializableXactHashLock
 *		- 同时保护 PredXact 与 SerializableXidHash。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/predicate.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * 接口例程
 *
 * 建立共享内存谓词锁结构的内务处理
 *		InitPredicateLocks(void)
 *		PredicateLockShmemSize(void)
 *
 * 谓词锁报告
 *		GetPredicateLockStatusData(void)
 *		PageIsPredicateLocked(Relation relation, BlockNumber blkno)
 *
 * 谓词锁维护
 *		GetSerializableTransactionSnapshot(Snapshot snapshot)
 *		SetSerializableTransactionSnapshot(Snapshot snapshot,
 *										   VirtualTransactionId *sourcevxid)
 *		RegisterPredicateLockingXid(void)
 *		PredicateLockRelation(Relation relation, Snapshot snapshot)
 *		PredicateLockPage(Relation relation, BlockNumber blkno,
 *						Snapshot snapshot)
 *		PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
 *						 TransactionId tuple_xid)
 *		PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
 *							   BlockNumber newblkno)
 *		PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
 *								 BlockNumber newblkno)
 *		TransferPredicateLocksToHeapRelation(Relation relation)
 *		ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe)
 *
 * 冲突检测（也可能触发回滚）
 *		CheckForSerializableConflictOut(Relation relation, TransactionId xid,
 *										Snapshot snapshot)
 *		CheckForSerializableConflictIn(Relation relation, ItemPointer tid,
 *									   BlockNumber blkno)
 *		CheckTableForSerializableConflictIn(Relation relation)
 *
 * 最终回滚检查
 *		PreCommit_CheckForSerializationFailure(void)
 *
 * 两阶段提交支持
 *		AtPrepare_PredicateLocks(void);
 *		PostPrepare_PredicateLocks(TransactionId xid);
 *		PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit);
 *		predicatelock_twophase_recover(TransactionId xid, uint16 info,
 *									   void *recdata, uint32 len);
 */

#include "postgres.h"

#include "access/parallel.h"
#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_lfind.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/predicate_internals.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* 取消下一行注释以测试平稳降级代码。 */
/* #define TEST_SUMMARIZE_SERIAL */

/*
 * 为性能起见，先测试选择性最强的字段。
 *
 * 若下列条件全部成立，则 a 被 b 覆盖：
 *	1) a.database = b.database
 *	2) a.relation = b.relation
 *	3) b.offset 无效（b 为页粒度或更粗）
 *	4) 下列二者之一：
 *		4a) a.offset 有效（a 为元组粒度）且 a.page = b.page
 *	 或 4b) a.offset 无效且 b.page 无效（a 为
 *			页粒度，而 b 为关系粒度
 */
#define TargetTagIsCoveredBy(covered_target, covering_target)			\
	((GET_PREDICATELOCKTARGETTAG_RELATION(covered_target) == /* (2) */	\
	  GET_PREDICATELOCKTARGETTAG_RELATION(covering_target))				\
	 && (GET_PREDICATELOCKTARGETTAG_OFFSET(covering_target) ==			\
		 InvalidOffsetNumber)								 /* (3) */	\
	 && (((GET_PREDICATELOCKTARGETTAG_OFFSET(covered_target) !=			\
		   InvalidOffsetNumber)								 /* (4a) */ \
		  && (GET_PREDICATELOCKTARGETTAG_PAGE(covering_target) ==		\
			  GET_PREDICATELOCKTARGETTAG_PAGE(covered_target)))			\
		 || ((GET_PREDICATELOCKTARGETTAG_PAGE(covering_target) ==		\
			  InvalidBlockNumber)							 /* (4b) */ \
			 && (GET_PREDICATELOCKTARGETTAG_PAGE(covered_target)		\
				 != InvalidBlockNumber)))								\
	 && (GET_PREDICATELOCKTARGETTAG_DB(covered_target) ==	 /* (1) */	\
		 GET_PREDICATELOCKTARGETTAG_DB(covering_target)))

/*
 * 谓词加锁的目标与锁共享散列表被分区，以
 * 降低争用。要确定给定目标属于哪个分区，
 * 用 PredicateLockTargetTagHashCode() 计算标签的散列码，然后
 * 套用下列宏之一。
 * 注意：NUM_PREDICATELOCK_PARTITIONS 必须是 2 的幂！
 */
#define PredicateLockHashPartition(hashcode) \
	((hashcode) % NUM_PREDICATELOCK_PARTITIONS)
#define PredicateLockHashPartitionLock(hashcode) \
	(&MainLWLockArray[PREDICATELOCK_MANAGER_LWLOCK_OFFSET + \
		PredicateLockHashPartition(hashcode)].lock)
#define PredicateLockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[PREDICATELOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

#define NPREDICATELOCKTARGETENTS() \
	mul_size(max_predicate_locks_per_xact, add_size(MaxBackends, max_prepared_xacts))

#define SxactIsOnFinishedList(sxact) (!dlist_node_is_detached(&(sxact)->finishedLink))

/*
 * 注意：sxact 一旦通过了
 * PreCommit_CheckForSerializationFailure，即便它并未使用
 * 两阶段提交（2PC），也会被标为 "prepared"。过了这一点就不能再中止。
 *
 * PREPARED 标志在提交后仍然置位，因此 SxactIsCommitted
 * 蕴含 SxactIsPrepared。
 */
#define SxactIsCommitted(sxact) (((sxact)->flags & SXACT_FLAG_COMMITTED) != 0)
#define SxactIsPrepared(sxact) (((sxact)->flags & SXACT_FLAG_PREPARED) != 0)
#define SxactIsRolledBack(sxact) (((sxact)->flags & SXACT_FLAG_ROLLED_BACK) != 0)
#define SxactIsDoomed(sxact) (((sxact)->flags & SXACT_FLAG_DOOMED) != 0)
#define SxactIsReadOnly(sxact) (((sxact)->flags & SXACT_FLAG_READ_ONLY) != 0)
#define SxactHasSummaryConflictIn(sxact) (((sxact)->flags & SXACT_FLAG_SUMMARY_CONFLICT_IN) != 0)
#define SxactHasSummaryConflictOut(sxact) (((sxact)->flags & SXACT_FLAG_SUMMARY_CONFLICT_OUT) != 0)
/*
 * 下面这个宏实际上表示指定事务有一条
 * 冲突出，*指向一个在它之前提交的事务*。很难
 * 把这层意思放进一个长度合理的名字里。
 */
#define SxactHasConflictOut(sxact) (((sxact)->flags & SXACT_FLAG_CONFLICT_OUT) != 0)
#define SxactIsDeferrableWaiting(sxact) (((sxact)->flags & SXACT_FLAG_DEFERRABLE_WAITING) != 0)
#define SxactIsROSafe(sxact) (((sxact)->flags & SXACT_FLAG_RO_SAFE) != 0)
#define SxactIsROUnsafe(sxact) (((sxact)->flags & SXACT_FLAG_RO_UNSAFE) != 0)
#define SxactIsPartiallyReleased(sxact) (((sxact)->flags & SXACT_FLAG_PARTIALLY_RELEASED) != 0)

/*
 * 计算与 PREDICATELOCKTARGETTAG 关联的散列码。
 *
 * 为避免不必要地重算散列码，我们尽量
 * 在每个函数里只做一次，然后按需传递。除了
 * 把散列码传给 hash_search_with_hash_value() 之外，还可以从中取出
 * 锁分区号。
 */
#define PredicateLockTargetTagHashCode(predicatelocktargettag) \
	get_hash_value(PredicateLockTargetHash, predicatelocktargettag)

/*
 * 给定谓词锁标签及其目标的散列，
 * 计算锁的散列。
 *
 * 为使散列码也依赖于事务，我们把 sxid
 * 结构的地址异或进散列码并左移，使得
 * 分区号那些位不变。既然这只是散列，
 * 地址高位丢失也无妨；用一个
 * 中间变量来抑制指针转整数的警告。
 */
#define PredicateLockHashCodeFromTargetHashCode(predicatelocktag, targethash) \
	((targethash) ^ ((uint32) PointerGetDatum((predicatelocktag)->myXact)) \
	 << LOG2_NUM_PREDICATELOCK_PARTITIONS)


/*
 * 用以访问旧 xid 的 SLRU 缓冲区。
 */
static SlruCtlData SerialSlruCtlData;

#define SerialSlruCtl			(&SerialSlruCtlData)

#define SERIAL_PAGESIZE			BLCKSZ
#define SERIAL_ENTRYSIZE			sizeof(SerCommitSeqNo)
#define SERIAL_ENTRIESPERPAGE	(SERIAL_PAGESIZE / SERIAL_ENTRYSIZE)

/*
 * 按跟踪全部事务所需的数量设置最大页数。
 */
#define SERIAL_MAX_PAGE			(MaxTransactionId / SERIAL_ENTRIESPERPAGE)

#define SerialNextPage(page) (((page) >= SERIAL_MAX_PAGE) ? 0 : (page) + 1)

#define SerialValue(slotno, xid) (*((SerCommitSeqNo *) \
	(SerialSlruCtl->shared->page_buffer[slotno] + \
	((((uint32) (xid)) % SERIAL_ENTRIESPERPAGE) * SERIAL_ENTRYSIZE))))

#define SerialPage(xid)	(((uint32) (xid)) / SERIAL_ENTRIESPERPAGE)

typedef struct SerialControlData
{
	int			headPage;		/* 最新已初始化的页 */
	TransactionId headXid;		/* SLRU 中最新的有效 Xid */
	TransactionId tailXid;		/* 我们可能关心的最旧 xmin */
}			SerialControlData;

typedef struct SerialControlData *SerialControl;

static SerialControl serialControl;

/*
 * 当 "finished" 列表上最旧的已提交事务被移入
 * SLRU 时，其谓词锁会移到这个 "dummy" 事务，
 * 并合并重复的目标。发现重复时，采用较晚的
 * commitSeqNo。
 */
static SERIALIZABLEXACT *OldCommittedSxact;


/*
 * 这些配置变量用来设置谓词锁表的大小，
 * 并控制把谓词锁提升到更粗的粒度，试图在
 * 内存压力下使性能平稳下降（主要体现为假阳性的序列化
 * 失败）。
 */
int			max_predicate_locks_per_xact;	/* 在 guc_tables.c 中 */
int			max_predicate_locks_per_relation;	/* 在 guc_tables.c 中 */
int			max_predicate_locks_per_page;	/* 在 guc_tables.c 中 */

/*
 * 它提供一份对象列表，用以跟踪那些
 * 参与谓词加锁的事务。列表中的条目大小固定，
 * 并驻留在共享内存中。条目的内存地址必须在其
 * 生命周期内保持不变。该列表的并发
 * 更新由外部保护；这段代码不负责管理这一点。
 * 列表中的条目数，以及每个条目允许的大小，
 * 在创建时即固定。
 */
static PredXactList PredXact;

/*
 * 它提供一个 RWConflict 数据元素池，用于事务之间的
 * 冲突列表。
 */
static RWConflictPoolHeader RWConflictPool;

/*
 * 谓词加锁散列表位于共享内存中。
 * 每个后端保存指向它们的指针。
 */
static HTAB *SerializableXidHash;
static HTAB *PredicateLockTargetHash;
static HTAB *PredicateLockHash;
static dlist_head *FinishedSerializableTransactions;

/*
 * PredicateLockTargetHash 中哑条目的标签。临时移除
 * 该条目，即可确保散列表中有足够的暂存空间来
 * 插入一个条目。这是一个否则无效的标签。
 */
static const PREDICATELOCKTARGETTAG ScratchTargetTag = {0, 0, 0, 0};
static uint32 ScratchTargetTagHash;
static LWLock *ScratchPartitionLock;

/*
 * 用于决定何时把多个细
 * 粒度锁合并成一把更粗粒度锁的本地散列表。
 */
static HTAB *LocalPredicateLockHash = NULL;

/*
 * 保存指向当前正在运行的可串行化事务（若有）的指针，
 * 以便快速引用。同时记住我们是否写过任何可能
 * 引起读写依赖（rw-conflict）的东西。
 */
static SERIALIZABLEXACT *MySerializableXact = InvalidSerializableXact;
static bool MyXactDidWrite = false;

/*
 * SXACT_FLAG_RO_UNSAFE 优化可能导致我们提前释放
 * MySerializableXact。若这发生在并行查询中，领导进程
 * 需要把 SERIALIZABLEXACT 的销毁推迟到事务
 * 结束，因为工作进程仍持有对它的引用。在那种
 * 情形下，领导进程把它存在这里。
 */
static SERIALIZABLEXACT *SavedSerializableXact = InvalidSerializableXact;

/* 局部函数 */

static SERIALIZABLEXACT *CreatePredXact(void);
static void ReleasePredXact(SERIALIZABLEXACT *sxact);

static bool RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer);
static void SetRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer);
static void SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact, SERIALIZABLEXACT *activeXact);
static void ReleaseRWConflict(RWConflict conflict);
static void FlagSxactUnsafe(SERIALIZABLEXACT *sxact);

static bool SerialPagePrecedesLogically(int page1, int page2);
static void SerialInit(void);
static void SerialAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo);
static SerCommitSeqNo SerialGetMinConflictCommitSeqNo(TransactionId xid);
static void SerialSetActiveSerXmin(TransactionId xid);

static uint32 predicatelock_hash(const void *key, Size keysize);
static void SummarizeOldestCommittedSxact(void);
static Snapshot GetSafeSnapshot(Snapshot origSnapshot);
static Snapshot GetSerializableTransactionSnapshotInt(Snapshot snapshot,
													  VirtualTransactionId *sourcevxid,
													  int sourcepid);
static bool PredicateLockExists(const PREDICATELOCKTARGETTAG *targettag);
static bool GetParentPredicateLockTag(const PREDICATELOCKTARGETTAG *tag,
									  PREDICATELOCKTARGETTAG *parent);
static bool CoarserLockCovers(const PREDICATELOCKTARGETTAG *newtargettag);
static void RemoveScratchTarget(bool lockheld);
static void RestoreScratchTarget(bool lockheld);
static void RemoveTargetIfNoLongerUsed(PREDICATELOCKTARGET *target,
									   uint32 targettaghash);
static void DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag);
static int	MaxPredicateChildLocks(const PREDICATELOCKTARGETTAG *tag);
static bool CheckAndPromotePredicateLockRequest(const PREDICATELOCKTARGETTAG *reqtag);
static void DecrementParentLocks(const PREDICATELOCKTARGETTAG *targettag);
static void CreatePredicateLock(const PREDICATELOCKTARGETTAG *targettag,
								uint32 targettaghash,
								SERIALIZABLEXACT *sxact);
static void DeleteLockTarget(PREDICATELOCKTARGET *target, uint32 targettaghash);
static bool TransferPredicateLocksToNewTarget(PREDICATELOCKTARGETTAG oldtargettag,
											  PREDICATELOCKTARGETTAG newtargettag,
											  bool removeOld);
static void PredicateLockAcquire(const PREDICATELOCKTARGETTAG *targettag);
static void DropAllPredicateLocksFromTable(Relation relation,
										   bool transfer);
static void SetNewSxactGlobalXmin(void);
static void ClearOldPredicateLocks(void);
static void ReleaseOneSerializableXact(SERIALIZABLEXACT *sxact, bool partial,
									   bool summarize);
static bool XidIsConcurrent(TransactionId xid);
static void CheckTargetForConflictsIn(PREDICATELOCKTARGETTAG *targettag);
static void FlagRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer);
static void OnConflict_CheckForSerializationFailure(const SERIALIZABLEXACT *reader,
													SERIALIZABLEXACT *writer);
static void CreateLocalPredicateLockHash(void);
static void ReleasePredicateLocksLocal(void);


/*------------------------------------------------------------------------*/

/*
 * 这个关系是否参与谓词加锁？临时关系与系统
 * 关系免于参与。
 */
static inline bool
PredicateLockingNeededForRelation(Relation relation)
{
	return !(relation->rd_id < FirstUnpinnedObjectId ||
			 RelationUsesLocalBuffers(relation));
}

/*
 * 当公共接口方法因读而被调用时，用这个测试
 * 判断是否应快速返回。
 *
 * 注意：此函数有副作用！若自上次调用以来该事务已被标为
 * RO-safe，我们就释放全部谓词锁并重置
 * MySerializableXact。这使得后续调用能够迅速返回。
 *
 * 把它标成 'inline'，是为了在
 * 不需要可串行化的常见情形下消除函数调用开销。
 */
static inline bool
SerializationNeededForRead(Relation relation, Snapshot snapshot)
{
	/* 若这不是可串行化事务，则无事可做 */
	if (MySerializableXact == InvalidSerializableXact)
		return false;

	/*
	 * 用特殊快照扫描时不要获取锁或检测冲突。
	 * 这排除了 CLUSTER 与 REINDEX 之类的操作。它们使用成批的
	 * 函数 TransferPredicateLocksToHeapRelation() 与
	 * CheckTableForSerializableConflictIn() 来参与可串行化，
	 * 但所涉及的扫描本身不需要可串行化。
	 */
	if (!IsMVCCSnapshot(snapshot))
		return false;

	/*
	 * 检查我们是否刚刚变成 "RO-safe"。若是，立即释放
	 * 全部锁，因为它们不再需要。这也会重置
	 * MySerializableXact，使后续对此函数的调用可以
	 * 迅速退出。
	 *
	 * 若所有并发的读/写事务
	 * 在提交时都没有指向更早快照的冲突出，事务就被标为 RO_SAFE，从而
	 * 确保本事务不可能发生冲突。
	 */
	if (SxactIsROSafe(MySerializableXact))
	{
		ReleasePredicateLocks(false, true);
		return false;
	}

	/* 检查该关系是否不参与谓词加锁 */
	if (!PredicateLockingNeededForRelation(relation))
		return false;

	return true;				/* 没有理由跳过谓词加锁 */
}

/*
 * 与 SerializationNeededForRead() 类似，但在写时调用。
 * 逻辑相同，但没有快照，并且我们不可能是 RO-safe。
 */
static inline bool
SerializationNeededForWrite(Relation relation)
{
	/* 若这不是可串行化事务，则无事可做 */
	if (MySerializableXact == InvalidSerializableXact)
		return false;

	/* 检查该关系是否不参与谓词加锁 */
	if (!PredicateLockingNeededForRelation(relation))
		return false;

	return true;				/* 没有理由跳过谓词加锁 */
}


/*------------------------------------------------------------------------*/

/*
 * 这些函数是针对这种特定
 * 结构类型的简单列表实现。若将来有通用的共享内存列表，
 * 大概应改用那个。
 */
static SERIALIZABLEXACT *
CreatePredXact(void)
{
	SERIALIZABLEXACT *sxact;

	if (dlist_is_empty(&PredXact->availableList))
		return NULL;

	sxact = dlist_container(SERIALIZABLEXACT, xactLink,
							dlist_pop_head_node(&PredXact->availableList));
	dlist_push_tail(&PredXact->activeList, &sxact->xactLink);
	return sxact;
}

static void
ReleasePredXact(SERIALIZABLEXACT *sxact)
{
	Assert(ShmemAddrIsValid(sxact));

	dlist_delete(&sxact->xactLink);
	dlist_push_tail(&PredXact->availableList, &sxact->xactLink);
}

/*------------------------------------------------------------------------*/

/*
 * 这些函数管理对 RWConflict 池和列表的基本访问。
 */
static bool
RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer)
{
	dlist_iter	iter;

	Assert(reader != writer);

	/* 先检查所称冲突的两端。 */
	if (SxactIsDoomed(reader)
		|| SxactIsDoomed(writer)
		|| dlist_is_empty(&reader->outConflicts)
		|| dlist_is_empty(&writer->inConflicts))
		return false;

	/*
	 * 可能存在冲突；遍历列表以查明。
	 *
	 * 需要 unconstify，因为我们没有 const 版本的
	 * dlist_foreach()。
	 */
	dlist_foreach(iter, &unconstify(SERIALIZABLEXACT *, reader)->outConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, outLink, iter.cur);

		if (conflict->sxactIn == writer)
			return true;
	}

	/* 未发现冲突。 */
	return false;
}

static void
SetRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer)
{
	RWConflict	conflict;

	Assert(reader != writer);
	Assert(!RWConflictExists(reader, writer));

	if (dlist_is_empty(&RWConflictPool->availableList))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase max_connections.")));

	conflict = dlist_head_element(RWConflictData, outLink, &RWConflictPool->availableList);
	dlist_delete(&conflict->outLink);

	conflict->sxactOut = reader;
	conflict->sxactIn = writer;
	dlist_push_tail(&reader->outConflicts, &conflict->outLink);
	dlist_push_tail(&writer->inConflicts, &conflict->inLink);
}

static void
SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact,
						  SERIALIZABLEXACT *activeXact)
{
	RWConflict	conflict;

	Assert(roXact != activeXact);
	Assert(SxactIsReadOnly(roXact));
	Assert(!SxactIsReadOnly(activeXact));

	if (dlist_is_empty(&RWConflictPool->availableList))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a potential read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase max_connections.")));

	conflict = dlist_head_element(RWConflictData, outLink, &RWConflictPool->availableList);
	dlist_delete(&conflict->outLink);

	conflict->sxactOut = activeXact;
	conflict->sxactIn = roXact;
	dlist_push_tail(&activeXact->possibleUnsafeConflicts, &conflict->outLink);
	dlist_push_tail(&roXact->possibleUnsafeConflicts, &conflict->inLink);
}

static void
ReleaseRWConflict(RWConflict conflict)
{
	dlist_delete(&conflict->inLink);
	dlist_delete(&conflict->outLink);
	dlist_push_tail(&RWConflictPool->availableList, &conflict->outLink);
}

static void
FlagSxactUnsafe(SERIALIZABLEXACT *sxact)
{
	dlist_mutable_iter iter;

	Assert(SxactIsReadOnly(sxact));
	Assert(!SxactIsROSafe(sxact));

	sxact->flags |= SXACT_FLAG_RO_UNSAFE;

	/*
	 * 已知这不是安全快照，因此可以停止寻找其他
	 * 潜在冲突。
	 */
	dlist_foreach_modify(iter, &sxact->possibleUnsafeConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		Assert(!SxactIsReadOnly(conflict->sxactOut));
		Assert(sxact == conflict->sxactIn);

		ReleaseRWConflict(conflict);
	}
}

/*------------------------------------------------------------------------*/

/*
 * 判断一个 Serial 页号就截断而言是否 "更旧"。
 * 类似于 CLOGPagePrecedes()。
 */
static bool
SerialPagePrecedesLogically(int page1, int page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * SERIAL_ENTRIESPERPAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * SERIAL_ENTRIESPERPAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + SERIAL_ENTRIESPERPAGE - 1));
}

#ifdef USE_ASSERT_CHECKING
static void
SerialPagePrecedesLogicallyUnitTests(void)
{
	int			per_page = SERIAL_ENTRIESPERPAGE,
				offset = per_page / 2;
	int			newestPage,
				oldestPage,
				headPage,
				targetPage;
	TransactionId newestXact,
				oldestXact;

	/* GetNewTransactionId() 已分配它能安全使用的最后一个 XID。 */
	newestPage = 2 * SLRU_PAGES_PER_SEGMENT - 1;	/* 并无特殊之处 */
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1U << 31;
	oldestPage = oldestXact / per_page;

	/*
	 * 在此情形下，SLRU 的 headPage 对应最后约 1000 个已分配的
	 * XID。oldestXact 结束时，自它
	 * 开始以来已过去约 20 亿个 XID。后续事务会使我们把 oldestXact 汇总到
	 * tailPage。函数必须返回 false，这样 SerialAdd() 才不会把
	 * tailPage（其中可能含有其他较旧、最近才结束的
	 * XID 的条目）以及一半 SLRU 清零。要走到这里，需要在
	 * 单用户模式下消耗约 20 亿个 XID，可能性可忽略。
	 */
	headPage = newestPage;
	targetPage = oldestPage;
	Assert(!SerialPagePrecedesLogically(headPage, targetPage));

	/*
	 * 在此情形下，SLRU 的 headPage 对应 oldestXact。我们正在
	 * 汇总一个接近 newestXact 的 XID。（假定很少有其他 XID 使用
	 * SERIALIZABLE，因此 headPage 推进极少。假定
	 * oldestXact 运行了很久，直到最近才进入 SLRU。）
	 * 函数必须返回 true，以使 SerialAdd() 创建 targetPage。
	 *
	 * 当前实现错误处理了这一情形，但还不值得
	 * 修复。通过断言对其前一页处理正确，验证缺陷只影响一页。
	 * 要走到这一情形，
	 * 需要在单用户模式下消耗约 20 亿个 XID，可能性
	 * 可忽略。而且即便发生，后果也
	 * 轻微，即一个新事务在 SimpleLruReadPage() 中失败。
	 */
	headPage = oldestPage;
	targetPage = newestPage;
	Assert(SerialPagePrecedesLogically(headPage, targetPage - 1));
#if 0
	Assert(SerialPagePrecedesLogically(headPage, targetPage));
#endif
}
#endif

/*
 * 为跟踪旧的已提交可串行化 xid 做初始化。
 */
static void
SerialInit(void)
{
	bool		found;

	/*
	 * 建立对 pg_serial 数据的 SLRU 管理。
	 */
	SerialSlruCtl->PagePrecedes = SerialPagePrecedesLogically;
	SimpleLruInit(SerialSlruCtl, "Serial",
				  NUM_SERIAL_BUFFERS, 0, SerialSLRULock, "pg_serial",
				  LWTRANCHE_SERIAL_BUFFER, SYNC_HANDLER_NONE);
#ifdef USE_ASSERT_CHECKING
	SerialPagePrecedesLogicallyUnitTests();
#endif
	SlruPagePrecedesUnitTests(SerialSlruCtl, SERIAL_ENTRIESPERPAGE);

	/*
	 * 创建或附着到 SerialControl 结构。
	 */
	serialControl = (SerialControl)
		ShmemInitStruct("SerialControlData", sizeof(SerialControlData), &found);

	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		/*
		 * 设置控制信息以反映空的 SLRU。
		 */
		serialControl->headPage = -1;
		serialControl->headXid = InvalidTransactionId;
		serialControl->tailXid = InvalidTransactionId;
	}
}

/*
 * 记录一个已提交的读写可串行化 xid，以及该 xid 曾对其有读写依赖（rw-conflict）出的
 * 任何事务的最小 commitSeqNo。
 * 无效的 commitSeqNo 表示该 xid 没有任何冲突出。
 */
static void
SerialAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo)
{
	TransactionId tailXid;
	int			targetPage;
	int			slotno;
	int			firstZeroPage;
	bool		isNewPage;

	Assert(TransactionIdIsValid(xid));

	targetPage = SerialPage(xid);

	LWLockAcquire(SerialSLRULock, LW_EXCLUSIVE);

	/*
	 * 若 'xid' 比全局 xmin（== tailXid）更旧，毕竟
	 * 就不必存储它。若把全局 xmin 往后拖的最旧事务刚刚
	 * 结束，使 'xid' 不再要紧，就会发生这种情况，但
	 * ClearOldPredicateLocks() 尚未运行。
	 */
	tailXid = serialControl->tailXid;
	if (!TransactionIdIsValid(tailXid) || TransactionIdPrecedes(xid, tailXid))
	{
		LWLockRelease(SerialSLRULock);
		return;
	}

	/*
	 * 若 SLRU 当前未使用，在投入使用前把从
	 * tailXid 到 headXid 的整个活动区清零。否则只把
	 * 随着我们推进而进入 tailXid-headXid 范围的新页清零
	 * headXid。
	 */
	if (serialControl->headPage < 0)
	{
		firstZeroPage = SerialPage(tailXid);
		isNewPage = true;
	}
	else
	{
		firstZeroPage = SerialNextPage(serialControl->headPage);
		isNewPage = SerialPagePrecedesLogically(serialControl->headPage,
												targetPage);
	}

	if (!TransactionIdIsValid(serialControl->headXid)
		|| TransactionIdFollows(xid, serialControl->headXid))
		serialControl->headXid = xid;
	if (isNewPage)
		serialControl->headPage = targetPage;

	if (isNewPage)
	{
		/* 初始化中间的页。 */
		while (firstZeroPage != targetPage)
		{
			(void) SimpleLruZeroPage(SerialSlruCtl, firstZeroPage);
			firstZeroPage = SerialNextPage(firstZeroPage);
		}
		slotno = SimpleLruZeroPage(SerialSlruCtl, targetPage);
	}
	else
		slotno = SimpleLruReadPage(SerialSlruCtl, targetPage, true, xid);

	SerialValue(slotno, xid) = minConflictCommitSeqNo;
	SerialSlruCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(SerialSLRULock);
}

/*
 * 取给定 xid 的任一冲突出的最小 commitSeqNo。对于
 * 存在但没有冲突出的事务，InvalidSerCommitSeqNo
 * 将被返回。
 */
static SerCommitSeqNo
SerialGetMinConflictCommitSeqNo(TransactionId xid)
{
	TransactionId headXid;
	TransactionId tailXid;
	SerCommitSeqNo val;
	int			slotno;

	Assert(TransactionIdIsValid(xid));

	LWLockAcquire(SerialSLRULock, LW_SHARED);
	headXid = serialControl->headXid;
	tailXid = serialControl->tailXid;
	LWLockRelease(SerialSLRULock);

	if (!TransactionIdIsValid(headXid))
		return 0;

	Assert(TransactionIdIsValid(tailXid));

	if (TransactionIdPrecedes(xid, tailXid)
		|| TransactionIdFollows(xid, headXid))
		return 0;

	/*
	 * 下面这个函数必须在不持有 SerialSLRULock 时调用，
	 * 但返回时会持有该锁，随后必须释放。
	 */
	slotno = SimpleLruReadPage_ReadOnly(SerialSlruCtl,
										SerialPage(xid), xid);
	val = SerialValue(slotno, xid);
	LWLockRelease(SerialSLRULock);
	return val;
}

/*
 * 每当活动的可串行化事务有了新的 xmin，就调用此函数。
 * 我们不必保留先于该 xmin 的事务的信息。
 * InvalidTransactionId 表示没有活动事务，因此
 * SLRU 中的一切都可以丢弃。
 */
static void
SerialSetActiveSerXmin(TransactionId xid)
{
	LWLockAcquire(SerialSLRULock, LW_EXCLUSIVE);

	/*
	 * 当没有活动的 sxact 时，没有任何重叠，把 xid 值设为
	 * 无效，以表明没有有效条目。不过不要清除 headPage。
	 * 新的 xmin 仍可能落在那一页上，我们不想
	 * 反复把同一页清零。
	 */
	if (!TransactionIdIsValid(xid))
	{
		serialControl->tailXid = InvalidTransactionId;
		serialControl->headXid = InvalidTransactionId;
		LWLockRelease(SerialSLRULock);
		return;
	}

	/*
	 * 恢复预备事务时，全局 xmin 可能
	 * 随恢复顺序而后退。通常这不可以，
	 * 但恢复期间没有可串行化事务会提交，因此
	 * SLRU 是空的，我们可以放过这一点。
	 */
	if (RecoveryInProgress())
	{
		Assert(serialControl->headPage < 0);
		if (!TransactionIdIsValid(serialControl->tailXid)
			|| TransactionIdPrecedes(xid, serialControl->tailXid))
		{
			serialControl->tailXid = xid;
		}
		LWLockRelease(SerialSLRULock);
		return;
	}

	Assert(!TransactionIdIsValid(serialControl->tailXid)
		   || TransactionIdFollows(xid, serialControl->tailXid));

	serialControl->tailXid = xid;

	LWLockRelease(SerialSLRULock);
}

/*
 * 执行检查点——无论是关闭期间，还是在线进行
 *
 * 我们没有任何需要在重启后存活的数据，但这里是
 * 截断 SLRU 的方便之处。
 */
void
CheckPointPredicate(void)
{
	int			tailPage;

	LWLockAcquire(SerialSLRULock, LW_EXCLUSIVE);

	/* 若 SLRU 当前未在使用，则迅速退出。 */
	if (serialControl->headPage < 0)
	{
		LWLockRelease(SerialSLRULock);
		return;
	}

	if (TransactionIdIsValid(serialControl->tailXid))
	{
		/* 可以把 SLRU 截断到包含 tailXid 的那一页 */
		tailPage = SerialPage(serialControl->tailXid);
	}
	else
	{
		/*----------
		 * SLRU 不再需要。在把 head 设为
		 * 无效。
		 *
		 * XXX：有可能直到发生 XID
		 * 回绕之前都不再需要 SLRU，于是我们留下的、包含 headPage 的段
		 * 会再次显得像是新的。那种情况下，要到
		 * XID 视界推进到足以使它
		 * 再次成为当前段，它才会被移除。
		 *
		 * XXX：这应当发生在 vac_truncate_clog() 中，而不是在检查点里。
		 * 考虑这一情形：从一个没有进行中
		 * 事务、且 VACUUM FREEZE 已把 oldestXact 推到最大的系统开始：
		 * - 启动一个 SERIALIZABLE 事务。
		 * - 启动、结束并汇总一个 SERIALIZABLE 事务，从而创建
		 *   一个 SLRU 页。
		 * - 消耗 XID 直到 xidStopLimit。
		 * - 结束全部事务。由于那个长时间运行的 SERIALIZABLE
		 *   事务，更早的检查点没有碰过 headPage。
		 *   下一次检查点会改动它，但那次检查点发生在
		 *   本情形结束之后。
		 * - VACUUM 以推进 XID 限制。
		 * - 消耗约 200 万个 XID，越过从前的 xidWrapLimit。
		 * - 启动、结束并汇总一个 SERIALIZABLE 事务。
		 *   SerialAdd() 拒绝创建 targetPage，因为 headPage
		 *   相对该 targetPage 不被视为处于过去。发起
		 *   汇总的事务失败于
		 *   SimpleLruReadPage()。
		 */
		tailPage = serialControl->headPage;
		serialControl->headPage = -1;
	}

	LWLockRelease(SerialSLRULock);

	/* 截掉不再需要的页 */
	SimpleLruTruncate(SerialSlruCtl, tailPage);

	/*
	 * 把脏的 SLRU 页写到磁盘
	 *
	 * 从正确性角度看这其实并不必要。我们
	 * 做它只是作为调试辅助。
	 *
	 * 我们在截断之后才做这件事，是为了避免在
	 * 删除它们所在的文件之前写那些页，那将完全
	 * 毫无意义。
	 */
	SimpleLruWriteAll(SerialSlruCtl, true);
}

/*------------------------------------------------------------------------*/

/*
 * InitPredicateLocks -- 初始化谓词加锁数据结构。
 *
 * 由 CreateSharedMemoryAndSemaphores() 调用，更多说明见该函数。
 * 正常的 postmaster 情形下，共享散列表
 * 在这里创建。后端经由 fork() 继承指向
 * 共享表的指针。在 EXEC_BACKEND 情形下，每个
 * 后端重新执行这段代码，以取得指向已经存在的
 * 共享散列表的指针。
 */
void
InitPredicateLocks(void)
{
	HASHCTL		info;
	long		max_table_size;
	Size		requestSize;
	bool		found;

#ifndef EXEC_BACKEND
	Assert(!IsUnderPostmaster);
#endif

	/*
	 * 计算谓词锁目标散列表的大小。注意这些
	 * 计算必须与 PredicateLockShmemSize 一致！
	 */
	max_table_size = NPREDICATELOCKTARGETENTS();

	/*
	 * 为 PREDICATELOCKTARGET 结构分配散列表。其中存放
	 * 每个谓词锁目标的信息。
	 */
	info.keysize = sizeof(PREDICATELOCKTARGETTAG);
	info.entrysize = sizeof(PREDICATELOCKTARGET);
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

	PredicateLockTargetHash = ShmemInitHash("PREDICATELOCKTARGET hash",
											max_table_size,
											max_table_size,
											&info,
											HASH_ELEM | HASH_BLOBS |
											HASH_PARTITION | HASH_FIXED_SIZE);

	/*
	 * 在散列表中保留一个哑条目；我们用它来确保在需要分裂或合并页时
	 * 总有一个条目可用，
	 * 因为那里空间耗尽可能意味着中止一个
	 * 非可串行化事务。
	 */
	if (!IsUnderPostmaster)
	{
		(void) hash_search(PredicateLockTargetHash, &ScratchTargetTag,
						   HASH_ENTER, &found);
		Assert(!found);
	}

	/* 预先计算暂存条目的散列与分区锁 */
	ScratchTargetTagHash = PredicateLockTargetTagHashCode(&ScratchTargetTag);
	ScratchPartitionLock = PredicateLockHashPartitionLock(ScratchTargetTagHash);

	/*
	 * 为 PREDICATELOCK 结构分配散列表。其中存放每个
	 * 事务在某目标上的锁的信息。
	 */
	info.keysize = sizeof(PREDICATELOCKTAG);
	info.entrysize = sizeof(PREDICATELOCK);
	info.hash = predicatelock_hash;
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

	/* 假定平均每个目标有 2 个事务 */
	max_table_size *= 2;

	PredicateLockHash = ShmemInitHash("PREDICATELOCK hash",
									  max_table_size,
									  max_table_size,
									  &info,
									  HASH_ELEM | HASH_FUNCTION |
									  HASH_PARTITION | HASH_FIXED_SIZE);

	/*
	 * 计算可串行化事务散列表的大小。注意这些
	 * 计算必须与 PredicateLockShmemSize 一致！
	 */
	max_table_size = (MaxBackends + max_prepared_xacts);

	/*
	 * 分配一个列表，用以保存参与
	 * 谓词加锁的事务的信息。
	 *
	 * 假定平均每个后端有 10 个谓词加锁事务。
	 * 这样就可以在细节仍在时积极清理，然后才必须
	 * 把数据汇总以便存入 SLRU 和 "dummy" 事务。
	 */
	max_table_size *= 10;

	PredXact = ShmemInitStruct("PredXactList",
							   PredXactListDataSize,
							   &found);
	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		int			i;

		dlist_init(&PredXact->availableList);
		dlist_init(&PredXact->activeList);
		PredXact->SxactGlobalXmin = InvalidTransactionId;
		PredXact->SxactGlobalXminCount = 0;
		PredXact->WritableSxactCount = 0;
		PredXact->LastSxactCommitSeqNo = FirstNormalSerCommitSeqNo - 1;
		PredXact->CanPartialClearThrough = 0;
		PredXact->HavePartialClearedThrough = 0;
		requestSize = mul_size((Size) max_table_size,
							   sizeof(SERIALIZABLEXACT));
		PredXact->element = ShmemAlloc(requestSize);
		/* 把全部元素加入可用列表，并清空。 */
		memset(PredXact->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			LWLockInitialize(&PredXact->element[i].perXactPredicateListLock,
							 LWTRANCHE_PER_XACT_PREDICATE_LIST);
			dlist_push_tail(&PredXact->availableList, &PredXact->element[i].xactLink);
		}
		PredXact->OldCommittedSxact = CreatePredXact();
		SetInvalidVirtualTransactionId(PredXact->OldCommittedSxact->vxid);
		PredXact->OldCommittedSxact->prepareSeqNo = 0;
		PredXact->OldCommittedSxact->commitSeqNo = 0;
		PredXact->OldCommittedSxact->SeqNo.lastCommitBeforeSnapshot = 0;
		dlist_init(&PredXact->OldCommittedSxact->outConflicts);
		dlist_init(&PredXact->OldCommittedSxact->inConflicts);
		dlist_init(&PredXact->OldCommittedSxact->predicateLocks);
		dlist_node_init(&PredXact->OldCommittedSxact->finishedLink);
		dlist_init(&PredXact->OldCommittedSxact->possibleUnsafeConflicts);
		PredXact->OldCommittedSxact->topXid = InvalidTransactionId;
		PredXact->OldCommittedSxact->finishedBefore = InvalidTransactionId;
		PredXact->OldCommittedSxact->xmin = InvalidTransactionId;
		PredXact->OldCommittedSxact->flags = SXACT_FLAG_COMMITTED;
		PredXact->OldCommittedSxact->pid = 0;
		PredXact->OldCommittedSxact->pgprocno = INVALID_PGPROCNO;
	}
	/* 这个值从不改变，因此保留一份本地副本。 */
	OldCommittedSxact = PredXact->OldCommittedSxact;

	/*
	 * 为 SERIALIZABLEXID 结构分配散列表。其中存放已访问数据的
	 * 可串行化事务的按 xid 信息。
	 */
	info.keysize = sizeof(SERIALIZABLEXIDTAG);
	info.entrysize = sizeof(SERIALIZABLEXID);

	SerializableXidHash = ShmemInitHash("SERIALIZABLEXID hash",
										max_table_size,
										max_table_size,
										&info,
										HASH_ELEM | HASH_BLOBS |
										HASH_FIXED_SIZE);

	/*
	 * 分配空间，以便在附于这些事务的列表中
	 * 跟踪读写依赖（rw-conflict）。
	 *
	 * 假定平均每个事务有 5 个冲突。计算表明
	 * 即便在最悲观的
	 * 负载下，直到 max_connections = 200 且全部 200 个连接都用可串行化事务冲击
	 * 数据库，这也足以防止资源耗尽。超过这一点，在试图标记冲突时
	 * 偶尔会有事务被取消。那
	 * 大概是可以接受的。
	 */
	max_table_size *= 5;

	RWConflictPool = ShmemInitStruct("RWConflictPool",
									 RWConflictPoolHeaderDataSize,
									 &found);
	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		int			i;

		dlist_init(&RWConflictPool->availableList);
		requestSize = mul_size((Size) max_table_size,
							   RWConflictDataSize);
		RWConflictPool->element = ShmemAlloc(requestSize);
		/* 把全部元素加入可用列表，并清空。 */
		memset(RWConflictPool->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			dlist_push_tail(&RWConflictPool->availableList,
							&RWConflictPool->element[i].outLink);
		}
	}

	/*
	 * 为已结束的可串行化事务列表创建或附着其头部
	 * 事务。
	 */
	FinishedSerializableTransactions = (dlist_head *)
		ShmemInitStruct("FinishedSerializableTransactions",
						sizeof(dlist_head),
						&found);
	Assert(found == IsUnderPostmaster);
	if (!found)
		dlist_init(FinishedSerializableTransactions);

	/*
	 * 初始化 SLRU，用以存放旧的已提交可串行化
	 * 事务。
	 */
	SerialInit();
}

/*
 * 估计谓词锁表所用的共享内存空间
 */
Size
PredicateLockShmemSize(void)
{
	Size		size = 0;
	long		max_table_size;

	/* 谓词锁目标散列表 */
	max_table_size = NPREDICATELOCKTARGETENTS();
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(PREDICATELOCKTARGET)));

	/* 谓词锁散列表 */
	max_table_size *= 2;
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(PREDICATELOCK)));

	/*
	 * 由于 NPREDICATELOCKTARGETENTS 只是估计，增加 10% 的安全
	 * 余量。
	 */
	size = add_size(size, size / 10);

	/* 事务列表 */
	max_table_size = MaxBackends + max_prepared_xacts;
	max_table_size *= 10;
	size = add_size(size, PredXactListDataSize);
	size = add_size(size, mul_size((Size) max_table_size,
								   sizeof(SERIALIZABLEXACT)));

	/* 事务 xid 表 */
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(SERIALIZABLEXID)));

	/* 读写依赖（rw-conflict）池 */
	max_table_size *= 5;
	size = add_size(size, RWConflictPoolHeaderDataSize);
	size = add_size(size, mul_size((Size) max_table_size,
								   RWConflictDataSize));

	/* 已结束的可串行化事务列表的头。 */
	size = add_size(size, sizeof(dlist_head));

	/* 用于 SLRU 跟踪旧的已提交 xid 的共享内存结构。 */
	size = add_size(size, sizeof(SerialControlData));
	size = add_size(size, SimpleLruShmemSize(NUM_SERIAL_BUFFERS, 0));

	return size;
}


/*
 * 计算与 PREDICATELOCKTAG 关联的散列码。
 *
 * 因为我们希望谓词锁目标与谓词锁这两张
 * PREDICATELOCKTARGET 和 PREDICATELOCK 散列表只用同一套分区锁，必须确保
 * PREDICATELOCK 落入与其
 * 关联的 PREDICATELOCKTARGET 相同的分区号。dynahash.c 期望分区号
 * 是散列码的低位，因此
 * PREDICATELOCKTAG 的散列码必须与关联的
 * PREDICATELOCKTARGETTAG 的散列码有相同的低位。我们用这个
 * 专用散列函数来做到这一点。
 */
static uint32
predicatelock_hash(const void *key, Size keysize)
{
	const PREDICATELOCKTAG *predicatelocktag = (const PREDICATELOCKTAG *) key;
	uint32		targethash;

	Assert(keysize == sizeof(PREDICATELOCKTAG));

	/* 查看关联的目标对象，并计算其散列码 */
	targethash = PredicateLockTargetTagHashCode(&predicatelocktag->myTarget->tag);

	return PredicateLockHashCodeFromTargetHashCode(predicatelocktag, targethash);
}


/*
 * GetPredicateLockStatusData
 *		返回一张表，其中包含谓词
 *		锁管理器的内部状态，供 pg_lock_status 使用。
 *
 * 与 GetLockStatusData 类似，本函数尽量缩短持有分区 LWLock 的
 * 时间，办法是返回两个数组，其中只
 * 包含每个锁表条目的 PREDICATELOCKTARGETTAG 与 SERIALIZABLEXACT。
 * 同一个 PREDICATELOCKTARGETTAG 和
 * SERIALIZABLEXACT 很可能出现多份。
 */
PredicateLockData *
GetPredicateLockStatusData(void)
{
	PredicateLockData *data;
	int			i;
	int			els,
				el;
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCK *predlock;

	data = (PredicateLockData *) palloc(sizeof(PredicateLockData));

	/*
	 * 为保持一致，按升序同时取得全部分区锁，
	 * 然后取得 SerializableXactHashLock。
	 */
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_SHARED);
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	/* 取得锁的数量，并分配大小合适的数组。 */
	els = hash_get_num_entries(PredicateLockHash);
	data->nelements = els;
	data->locktags = (PREDICATELOCKTARGETTAG *)
		palloc(sizeof(PREDICATELOCKTARGETTAG) * els);
	data->xacts = (SERIALIZABLEXACT *)
		palloc(sizeof(SERIALIZABLEXACT) * els);


	/* 扫描 PredicateLockHash 并复制内容 */
	hash_seq_init(&seqstat, PredicateLockHash);

	el = 0;

	while ((predlock = (PREDICATELOCK *) hash_seq_search(&seqstat)))
	{
		data->locktags[el] = predlock->tag.myTarget->tag;
		data->xacts[el] = *predlock->tag.myXact;
		el++;
	}

	Assert(el == els);

	/* 按相反顺序释放锁 */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));

	return data;
}

/*
 * 释放共享内存结构，办法是把最旧的 sxact（位于
 * SummarizeOldestCommittedSxact 队列前端的那个）推进汇总形式。
 * 每次调用恰好释放一个 SERIALIZABLEXACT 结构，也可能
 * 释放下列结构中的一个或多个：SERIALIZABLEXID、PREDICATELOCK、
 * PREDICATELOCKTARGET、RWConflictData。
 */
static void
SummarizeOldestCommittedSxact(void)
{
	SERIALIZABLEXACT *sxact;

	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);

	/*
	 * 只有在没有可用的 sxact 槽位时才调用本函数。
	 * 其中一些必定属于已经结束的旧事务，因此
	 * FinishedSerializableTransactions 列表里应当有可以
	 * 汇总的东西。不过存在竞态：在我们不
	 * 持有任何锁的期间，某个事务可能已经结束并清理了全部
	 * 已结束的 sxact 条目，从而释放了它们的 sxact 槽位。在
	 * 那种情况下，这里无事可做。调用者重试时会找到
	 * 由另一个后端释放的某个槽位。
	 */
	if (dlist_is_empty(FinishedSerializableTransactions))
	{
		LWLockRelease(SerializableFinishedListLock);
		return;
	}

	/*
	 * 从已结束列表上取第一个 sxact——这将是最早的
	 * 提交。把它从列表中移除。
	 */
	sxact = dlist_head_element(SERIALIZABLEXACT, finishedLink,
							   FinishedSerializableTransactions);
	dlist_delete_thoroughly(&sxact->finishedLink);

	/* 加入 SLRU 汇总信息。 */
	if (TransactionIdIsValid(sxact->topXid) && !SxactIsReadOnly(sxact))
		SerialAdd(sxact->topXid, SxactHasConflictOut(sxact)
				  ? sxact->SeqNo.earliestOutConflictCommit : InvalidSerCommitSeqNo);

	/* 汇总并释放细节。 */
	ReleaseOneSerializableXact(sxact, false, true);

	LWLockRelease(SerializableFinishedListLock);
}

/*
 * GetSafeSnapshot
 *		为 READ ONLY DEFERRABLE
 *		事务取得并登记一个快照。确保该快照是 "安全" 的，即
 *		在其上运行的只读事务可以可串行化地执行，
 *		而无需进一步检查。这需要等待并发
 *		事务完成，并且若其中某一个
 *		可能制造冲突，就用新快照重试。
 *
 *		与 GetSerializableTransactionSnapshot（本函数是它的子程序）
 *		一样，传入的 Snapshot 指针应引用一块可以安全传给 GetSnapshotData 的静态数据
 *		区。
 */
static Snapshot
GetSafeSnapshot(Snapshot origSnapshot)
{
	Snapshot	snapshot;

	Assert(XactReadOnly && XactDeferrable);

	while (true)
	{
		/*
		 * GetSerializableTransactionSnapshotInt 将调用
		 * GetSnapshotData，因此我们需要把调用者传给我们的静态快照区提供给它。
		 * 返回的指针实际上就是
		 * 传给它的同一个指针，但这里我们避免作此假定。
		 */
		snapshot = GetSerializableTransactionSnapshotInt(origSnapshot,
														 NULL, InvalidPid);

		if (MySerializableXact == InvalidSerializableXact)
			return snapshot;	/* 没有并发的读/写事务；它是安全的 */

		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		/*
		 * 等待并发事务结束。若其中一个
		 * 把我们标为已冲突，则提前停止。
		 */
		MySerializableXact->flags |= SXACT_FLAG_DEFERRABLE_WAITING;
		while (!(dlist_is_empty(&MySerializableXact->possibleUnsafeConflicts) ||
				 SxactIsROUnsafe(MySerializableXact)))
		{
			LWLockRelease(SerializableXactHashLock);
			ProcWaitForSignal(WAIT_EVENT_SAFE_SNAPSHOT);
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		}
		MySerializableXact->flags &= ~SXACT_FLAG_DEFERRABLE_WAITING;

		if (!SxactIsROUnsafe(MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			break;				/* 成功 */
		}

		LWLockRelease(SerializableXactHashLock);

		/* 否则需要重试... */
		ereport(DEBUG2,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg_internal("deferrable snapshot was unsafe; trying a new one")));
		ReleasePredicateLocks(false, false);
	}

	/*
	 * 现在我们有了安全快照，因此不必再做进一步检查。
	 */
	Assert(SxactIsROSafe(MySerializableXact));
	ReleasePredicateLocks(false, true);

	return snapshot;
}

/*
 * GetSafeSnapshotBlockingPids
 *		若指定进程当前阻塞在 GetSafeSnapshot 中，
 *		把阻塞它的全部进程的进程 ID
 *		写入调用者提供的缓冲区 output[]。列表在
 *		output_size 处截断，写入缓冲区的 PID 个数
 *		被返回。若给定 PID 当前并未阻塞
 *		在 GetSafeSnapshot 中，则返回零。
 */
int
GetSafeSnapshotBlockingPids(int blocked_pid, int *output, int output_size)
{
	int			num_written = 0;
	dlist_iter	iter;
	SERIALIZABLEXACT *blocking_sxact = NULL;

	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	/* 用线性搜索找到 blocked_pid 的 SERIALIZABLEXACT。 */
	dlist_foreach(iter, &PredXact->activeList)
	{
		SERIALIZABLEXACT *sxact =
			dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

		if (sxact->pid == blocked_pid)
		{
			blocking_sxact = sxact;
			break;
		}
	}

	/* 我们找到它了吗，并且它当前是否正在 GetSafeSnapshot 中等待？ */
	if (blocking_sxact != NULL && SxactIsDeferrableWaiting(blocking_sxact))
	{
		/* 遍历可能的不安全冲突列表，收集 PID。 */
		dlist_foreach(iter, &blocking_sxact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, inLink, iter.cur);

			output[num_written++] = possibleUnsafeConflict->sxactOut->pid;

			if (num_written >= output_size)
				break;
		}
	}

	LWLockRelease(SerializableXactHashLock);

	return num_written;
}

/*
 * 为当前事务取得一个可用的快照。
 *
 * 确保 MySerializableXact 中有一个 SERIALIZABLEXACT 引用。
 * 它应对本进程是当前的，并且包含在 PredXact 中。
 *
 * 传入的 Snapshot 指针应引用一块静态数据区，可以
 * 安全地传给 GetSnapshotData。返回值实际上
 * 始终是这同一个指针；不分配新的快照数据结构
 * 于本函数内。
 */
Snapshot
GetSerializableTransactionSnapshot(Snapshot snapshot)
{
	Assert(IsolationIsSerializable());

	/*
	 * 恢复仍在进行时不能使用可串行化模式，例如
	 * 在热备上就是如此。即便有 check_transaction_isolation()
	 * 的检查，若 default_transaction_isolation 设为 serializable，
	 * 仍可能到达这里，故提示措辞要针对该情况。
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use serializable mode in a hot standby"),
				 errdetail("\"default_transaction_isolation\" is set to \"serializable\"."),
				 errhint("You can use \"SET default_transaction_isolation = 'repeatable read'\" to change the default.")));

	/*
	 * 对 SERIALIZABLE READ ONLY
	 * DEFERRABLE 事务有一项特殊优化——我们可以等待一个合适的快照，
	 * 从而在它开始运行后避免全部 SSI 开销。
	 */
	if (XactReadOnly && XactDeferrable)
		return GetSafeSnapshot(snapshot);

	return GetSerializableTransactionSnapshotInt(snapshot,
												 NULL, InvalidPid);
}

/*
 * 导入一个供当前事务使用的快照。
 *
 * 这与 GetSerializableTransactionSnapshot 几乎相同，只是
 * 我们不取新快照，而是使用交给我们的数据。
 *
 * 调用者必须已经验证该快照来自一个可串行化
 * 事务；并且若我们是读写的，源事务不得是
 * 只读的。
 */
void
SetSerializableTransactionSnapshot(Snapshot snapshot,
								   VirtualTransactionId *sourcevxid,
								   int sourcepid)
{
	Assert(IsolationIsSerializable());

	/*
	 * 若这是由 parallel.c 在并行工作进程中调用的，我们还不想
	 * 创建 SERIALIZABLEXACT，因为领导的
	 * SERIALIZABLEXACT 将由 AttachSerializableXact() 安装。我们
	 * 也不想在这种
	 * 情形下拒绝 SERIALIZABLE READ ONLY DEFERRABLE，因为领导已经判定它传给
	 * 我们的快照是安全的。因此我们无事可做。
	 */
	if (IsParallelWorker())
		return;

	/*
	 * 我们不允许 SERIALIZABLE READ ONLY DEFERRABLE 事务
	 * 导入快照，因为当我们使用被告知要用的快照时，没有办法等待安全快照。
	 * （XXX 与其抛出错误，
	 * 我们可否直接忽略 XactDeferrable 标志？）
	 */
	if (XactReadOnly && XactDeferrable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a snapshot-importing transaction must not be READ ONLY DEFERRABLE")));

	(void) GetSerializableTransactionSnapshotInt(snapshot, sourcevxid,
												 sourcepid);
}

/*
 * GetSerializableTransactionSnapshot 的核心
 *
 * 若 sourcevxid 有效，这实际上是导入操作，我们应当
 * 跳过调用 GetSnapshotData，因为快照内容已经
 * 装入。不过：为避免竞态，我们必须在取得 SerializableXactHashLock 之后
 * 检查源事务仍在运行。
 * 我们通过调用 ProcArrayInstallImportedXmin 来做这件事。
 */
static Snapshot
GetSerializableTransactionSnapshotInt(Snapshot snapshot,
									  VirtualTransactionId *sourcevxid,
									  int sourcepid)
{
	PGPROC	   *proc;
	VirtualTransactionId vxid;
	SERIALIZABLEXACT *sxact,
			   *othersxact;

	/* 我们只对可串行化事务做这件事。只做一次。 */
	Assert(MySerializableXact == InvalidSerializableXact);

	Assert(!RecoveryInProgress());

	/*
	 * 由于可串行化事务的所有部分必须使用同一个
	 * 快照，在并行操作
	 * 开始之后再建立一个就太晚了。
	 */
	if (IsInParallelMode())
		elog(ERROR, "cannot establish serializable snapshot during a parallel operation");

	proc = MyProc;
	Assert(proc != NULL);
	GET_VXID_FROM_PGPROC(vxid, *proc);

	/*
	 * 首先取得 sxact 结构，这可能涉及循环，并访问
	 * "finished" 列表以释放一个结构供使用。
	 *
	 * 在取得或检查快照时必须持有 SerializableXactHashLock，
	 * 以避免竞态，原因与
	 * GetSnapshotData 取得 ProcArrayLock 大致相同。由于我们可能必须
	 * 释放 SerializableXactHashLock 才能调用 SummarizeOldestCommittedSxact，
	 * 这意味着我们必须先创建 sxact，这有点烦人
	 * （特别是，procarray.c 中的 elog(ERROR) 会导致我们泄漏
	 * 该 sxact）。考虑重构以避免这一点。
	 */
#ifdef TEST_SUMMARIZE_SERIAL
	SummarizeOldestCommittedSxact();
#endif
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
	do
	{
		sxact = CreatePredXact();
		/* 若为空，把已提交的 sxact 推入 SLRU 汇总并重试。 */
		if (!sxact)
		{
			LWLockRelease(SerializableXactHashLock);
			SummarizeOldestCommittedSxact();
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		}
	} while (!sxact);

	/* 取得快照，或检查它是否可以安全使用 */
	if (!sourcevxid)
		snapshot = GetSnapshotData(snapshot);
	else if (!ProcArrayInstallImportedXmin(snapshot->xmin, sourcevxid))
	{
		ReleasePredXact(sxact);
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
				 errdetail("The source process with PID %d is not running anymore.",
						   sourcepid)));
	}

	/*
	 * 若没有非只读的可串行化事务，我们就可以对
	 * 只读事务 "退出" 谓词加锁与
	 * 冲突检查。
	 *
	 * 这样做是安全的，因为只读事务只有在它重叠一个可写事务、
	 * 而该可写事务又重叠一个在
	 * 只读事务开始之前就已提交的可写事务时，才会成为
	 * 危险结构的一部分。新的可写事务可以
	 * 与本事务重叠，但它无法满足另一个条件，即重叠
	 * 一个在本事务开始之前就已提交的事务。
	 */
	if (XactReadOnly && PredXact->WritableSxactCount == 0)
	{
		ReleasePredXact(sxact);
		LWLockRelease(SerializableXactHashLock);
		return snapshot;
	}

	/* 初始化该结构。 */
	sxact->vxid = vxid;
	sxact->SeqNo.lastCommitBeforeSnapshot = PredXact->LastSxactCommitSeqNo;
	sxact->prepareSeqNo = InvalidSerCommitSeqNo;
	sxact->commitSeqNo = InvalidSerCommitSeqNo;
	dlist_init(&(sxact->outConflicts));
	dlist_init(&(sxact->inConflicts));
	dlist_init(&(sxact->possibleUnsafeConflicts));
	sxact->topXid = GetTopTransactionIdIfAny();
	sxact->finishedBefore = InvalidTransactionId;
	sxact->xmin = snapshot->xmin;
	sxact->pid = MyProcPid;
	sxact->pgprocno = MyProc->pgprocno;
	dlist_init(&sxact->predicateLocks);
	dlist_node_init(&sxact->finishedLink);
	sxact->flags = 0;
	if (XactReadOnly)
	{
		dlist_iter	iter;

		sxact->flags |= SXACT_FLAG_READ_ONLY;

		/*
		 * 把所有并发的读/写事务登记为可能的冲突；若
		 * 它们全部提交时都没有任何指向更早
		 * 事务的引出冲突，则此快照可视为安全（我们就可以
		 * 不再跟踪谓词锁而运行）。
		 */
		dlist_foreach(iter, &PredXact->activeList)
		{
			othersxact = dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

			if (!SxactIsCommitted(othersxact)
				&& !SxactIsDoomed(othersxact)
				&& !SxactIsReadOnly(othersxact))
			{
				SetPossibleUnsafeConflict(sxact, othersxact);
			}
		}

		/*
		 * 若因为每个
		 * 未提交的可写事务结果都已注定失败，而我们没有找到任何可能不安全的冲突，则可以
		 * 立即 "退出"。见前面关于
		 * PredXact->WritableSxactCount == 0 的检查的注释。
		 */
		if (dlist_is_empty(&sxact->possibleUnsafeConflicts))
		{
			ReleasePredXact(sxact);
			LWLockRelease(SerializableXactHashLock);
			return snapshot;
		}
	}
	else
	{
		++(PredXact->WritableSxactCount);
		Assert(PredXact->WritableSxactCount <=
			   (MaxBackends + max_prepared_xacts));
	}

	/* 维护可串行化的全局 xmin 信息。 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount == 0);
		PredXact->SxactGlobalXmin = snapshot->xmin;
		PredXact->SxactGlobalXminCount = 1;
		SerialSetActiveSerXmin(snapshot->xmin);
	}
	else if (TransactionIdEquals(snapshot->xmin, PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount > 0);
		PredXact->SxactGlobalXminCount++;
	}
	else
	{
		Assert(TransactionIdFollows(snapshot->xmin, PredXact->SxactGlobalXmin));
	}

	MySerializableXact = sxact;
	MyXactDidWrite = false;		/* 还没有写过任何东西 */

	LWLockRelease(SerializableXactHashLock);

	CreateLocalPredicateLockHash();

	return snapshot;
}

static void
CreateLocalPredicateLockHash(void)
{
	HASHCTL		hash_ctl;

	/* 初始化父锁的后端本地散列表 */
	Assert(LocalPredicateLockHash == NULL);
	hash_ctl.keysize = sizeof(PREDICATELOCKTARGETTAG);
	hash_ctl.entrysize = sizeof(LOCALPREDICATELOCK);
	LocalPredicateLockHash = hash_create("Local predicate lock",
										 max_predicate_locks_per_xact,
										 &hash_ctl,
										 HASH_ELEM | HASH_BLOBS);
}

/*
 * 在 SerializableXidHash 中登记顶层 XID。
 * 并把它存入 MySerializableXact 以便引用。
 */
void
RegisterPredicateLockingXid(TransactionId xid)
{
	SERIALIZABLEXIDTAG sxidtag;
	SERIALIZABLEXID *sxid;
	bool		found;

	/*
	 * 若我们没有在为这个事务跟踪谓词锁数据，就应当
	 * 忽略该请求并迅速返回。
	 */
	if (MySerializableXact == InvalidSerializableXact)
		return;

	/* 我们应当有一个有效的 XID，并且处于顶层。 */
	Assert(TransactionIdIsValid(xid));

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* 每个事务只应做一次。 */
	Assert(MySerializableXact->topXid == InvalidTransactionId);

	MySerializableXact->topXid = xid;

	sxidtag.xid = xid;
	sxid = (SERIALIZABLEXID *) hash_search(SerializableXidHash,
										   &sxidtag,
										   HASH_ENTER, &found);
	Assert(!found);

	/* 初始化该结构。 */
	sxid->myXact = MySerializableXact;
	LWLockRelease(SerializableXactHashLock);
}


/*
 * 检查是否有任何事务对给定块号上的页
 * 持有谓词锁。
 *
 * 注意该事务可能已经完成，但由于重叠的可串行化事务
 * 尚未进入清理。无论事务隔离级别如何，这里都必须
 * 返回有效信息。
 *
 * 还要注意，这不检查冲突的关系锁，
 * 只检查专门加在给定页上的锁。
 *
 * 用途之一是在 GiST 索引清理期间支持正确行为。
 */
bool
PageIsPredicateLocked(Relation relation, BlockNumber blkno)
{
	PREDICATELOCKTARGETTAG targettag;
	uint32		targettaghash;
	LWLock	   *partitionLock;
	PREDICATELOCKTARGET *target;

	SET_PREDICATELOCKTARGETTAG_PAGE(targettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									blkno);

	targettaghash = PredicateLockTargetTagHashCode(&targettag);
	partitionLock = PredicateLockHashPartitionLock(targettaghash);
	LWLockAcquire(partitionLock, LW_SHARED);
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									&targettag, targettaghash,
									HASH_FIND, NULL);
	LWLockRelease(partitionLock);

	return (target != NULL);
}


/*
 * 检查本事务是否持有某一把锁。
 *
 * 重要提示：即便锁正被持有，本函数也可能返回 false，
 * 因为它使用本地锁表，而若另一事务修改了我们的锁列表（例如
 * 分裂索引页），本地锁表不会
 * 更新。当持有覆盖此目标的更粗
 * 粒度锁时，它也可能返回 true。请小心
 * 请只在此类错误
 * 可以接受的情形下使用本函数！
 */
static bool
PredicateLockExists(const PREDICATELOCKTARGETTAG *targettag)
{
	LOCALPREDICATELOCK *lock;

	/* 检查本地散列表 */
	lock = (LOCALPREDICATELOCK *) hash_search(LocalPredicateLockHash,
											  targettag,
											  HASH_FIND, NULL);

	if (!lock)
		return false;

	/*
	 * 在表中找到了条目，但仍需检查它是否真正
	 * 被持有——它可能只是某把被持有的锁的父项。
	 */
	return lock->held;
}

/*
 * 返回锁层次中的父锁标签：覆盖所提供标签的下一把更粗的
 * 锁。
 *
 * 若存在父标签则返回 true 并设置 *parent，
 * 若不存在则返回 false。
 */
static bool
GetParentPredicateLockTag(const PREDICATELOCKTARGETTAG *tag,
						  PREDICATELOCKTARGETTAG *parent)
{
	switch (GET_PREDICATELOCKTARGETTAG_TYPE(*tag))
	{
		case PREDLOCKTAG_RELATION:
			/* 关系锁没有父锁 */
			return false;

		case PREDLOCKTAG_PAGE:
			/* 父锁是关系锁 */
			SET_PREDICATELOCKTARGETTAG_RELATION(*parent,
												GET_PREDICATELOCKTARGETTAG_DB(*tag),
												GET_PREDICATELOCKTARGETTAG_RELATION(*tag));

			return true;

		case PREDLOCKTAG_TUPLE:
			/* 父锁是页锁 */
			SET_PREDICATELOCKTARGETTAG_PAGE(*parent,
											GET_PREDICATELOCKTARGETTAG_DB(*tag),
											GET_PREDICATELOCKTARGETTAG_RELATION(*tag),
											GET_PREDICATELOCKTARGETTAG_PAGE(*tag));
			return true;
	}

	/* 不可达 */
	Assert(false);
	return false;
}

/*
 * 检查我们正在考虑的锁是否已被本事务的一把
 * 更粗的锁覆盖。
 *
 * 与 PredicateLockExists 类似，本函数可能返回假
 * 阴性，但绝不会返回假阳性。
 */
static bool
CoarserLockCovers(const PREDICATELOCKTARGETTAG *newtargettag)
{
	PREDICATELOCKTARGETTAG targettag,
				parenttag;

	targettag = *newtargettag;

	/* 迭代检查父项，直到没有更多父项 */
	while (GetParentPredicateLockTag(&targettag, &parenttag))
	{
		targettag = parenttag;
		if (PredicateLockExists(&targettag))
			return true;
	}

	/* 没有更多父项可检查；锁未被覆盖 */
	return false;
}

/*
 * 从谓词锁目标散列中移除哑条目，以腾出一些
 * 暂存空间。调用者必须持有 SerializablePredicateListLock，
 * 并必须在释放该锁之前用 RestoreScratchTarget()
 * 恢复该条目。
 *
 * 若 lockheld 为 true，调用者已经持有包含暂存条目的那个分区的
 * 分区锁。
 */
static void
RemoveScratchTarget(bool lockheld)
{
	bool		found;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	if (!lockheld)
		LWLockAcquire(ScratchPartitionLock, LW_EXCLUSIVE);
	hash_search_with_hash_value(PredicateLockTargetHash,
								&ScratchTargetTag,
								ScratchTargetTagHash,
								HASH_REMOVE, &found);
	Assert(found);
	if (!lockheld)
		LWLockRelease(ScratchPartitionLock);
}

/*
 * 把哑条目重新插入谓词锁目标散列。
 */
static void
RestoreScratchTarget(bool lockheld)
{
	bool		found;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	if (!lockheld)
		LWLockAcquire(ScratchPartitionLock, LW_EXCLUSIVE);
	hash_search_with_hash_value(PredicateLockTargetHash,
								&ScratchTargetTag,
								ScratchTargetTagHash,
								HASH_ENTER, &found);
	Assert(!found);
	if (!lockheld)
		LWLockRelease(ScratchPartitionLock);
}

/*
 * 检查某个谓词锁目标的相关谓词锁列表是否为空，
 * 若为空则移除该目标。
 */
static void
RemoveTargetIfNoLongerUsed(PREDICATELOCKTARGET *target, uint32 targettaghash)
{
	PREDICATELOCKTARGET *rmtarget PG_USED_FOR_ASSERTS_ONLY;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	/* 在该目标上不再有锁之前，不能移除它。 */
	if (!dlist_is_empty(&target->predicateLocks))
		return;

	/* 实际移除该目标。 */
	rmtarget = hash_search_with_hash_value(PredicateLockTargetHash,
										   &target->tag,
										   targettaghash,
										   HASH_REMOVE, NULL);
	Assert(rmtarget == target);
}

/*
 * 删除本进程拥有的子目标锁。
 * 本实现假定每个目标标签字段的用法
 * 是统一的。若非必要，不必把这件事做复杂。
 *
 * 并行模式下我们取得一把 LWLock，因为工作进程
 * 后端可以访问领导的 SERIALIZABLEXACT。否则，
 * 我们不为谓词锁或锁
 * 目标结构取得 LWLock，除非我们打算
 * 去修改它们，因为不允许其他进程修改我们的
 * 锁。
 */
static void
DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag)
{
	SERIALIZABLEXACT *sxact;
	PREDICATELOCK *predlock;
	dlist_mutable_iter iter;

	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	sxact = MySerializableXact;
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);

	dlist_foreach_modify(iter, &sxact->predicateLocks)
	{
		PREDICATELOCKTAG oldlocktag;
		PREDICATELOCKTARGET *oldtarget;
		PREDICATELOCKTARGETTAG oldtargettag;

		predlock = dlist_container(PREDICATELOCK, xactLink, iter.cur);

		oldlocktag = predlock->tag;
		Assert(oldlocktag.myXact == sxact);
		oldtarget = oldlocktag.myTarget;
		oldtargettag = oldtarget->tag;

		if (TargetTagIsCoveredBy(oldtargettag, *newtargettag))
		{
			uint32		oldtargettaghash;
			LWLock	   *partitionLock;
			PREDICATELOCK *rmpredlock PG_USED_FOR_ASSERTS_ONLY;

			oldtargettaghash = PredicateLockTargetTagHashCode(&oldtargettag);
			partitionLock = PredicateLockHashPartitionLock(oldtargettaghash);

			LWLockAcquire(partitionLock, LW_EXCLUSIVE);

			dlist_delete(&predlock->xactLink);
			dlist_delete(&predlock->targetLink);
			rmpredlock = hash_search_with_hash_value
				(PredicateLockHash,
				 &oldlocktag,
				 PredicateLockHashCodeFromTargetHashCode(&oldlocktag,
														 oldtargettaghash),
				 HASH_REMOVE, NULL);
			Assert(rmpredlock == predlock);

			RemoveTargetIfNoLongerUsed(oldtarget, oldtargettaghash);

			LWLockRelease(partitionLock);

			DecrementParentLocks(&oldtargettag);
		}
	}
	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * 返回给定谓词锁目标的提升上限。这是
 * 在提升到指定标签之前允许的后代锁最大数量。
 * 注意该上限包括非直接后代（例如关系锁下的元组
 * 和页）。
 *
 * 目前页锁的默认上限是 2，关系锁则是下列值的一半：
 * max_pred_locks_per_transaction - 1，以便在升级时匹配
 * 更早版本的行为。
 *
 * TODO SSI：我们大概应当再增加一些 GUC，以允许最大比例，
 * 按关系中的页数限制页锁与元组锁，以及最大
 * 的一页内元组锁相对元组数的比例。这样会提供更
 * 均衡地把锁分配到它们最有用的地方，
 * 同时仍然用绝对数量防止一个关系
 * 占尽全部谓词锁资源。
 */
static int
MaxPredicateChildLocks(const PREDICATELOCKTARGETTAG *tag)
{
	switch (GET_PREDICATELOCKTARGETTAG_TYPE(*tag))
	{
		case PREDLOCKTAG_RELATION:
			return max_predicate_locks_per_relation < 0
				? (max_predicate_locks_per_xact
				   / (-max_predicate_locks_per_relation)) - 1
				: max_predicate_locks_per_relation;

		case PREDLOCKTAG_PAGE:
			return max_predicate_locks_per_page;

		case PREDLOCKTAG_TUPLE:

			/*
			 * 不可达：没有比元组更细的粒度，因此我们
			 * 绝不应当试图提升到它。
			 */
			Assert(false);
			return 0;
	}

	/* 不可达 */
	Assert(false);
	return 0;
}

/*
 * 对新取得的谓词锁的全部祖先，递增
 * 它们在父散列表中的子计数。若其中任何一个的
 * 后代数超过其提升阈值，就取得
 * 最粗的那把这样的锁。
 *
 * 若取得了父锁则返回 true，否则返回 false。
 */
static bool
CheckAndPromotePredicateLockRequest(const PREDICATELOCKTARGETTAG *reqtag)
{
	PREDICATELOCKTARGETTAG targettag,
				nexttag,
				promotiontag;
	LOCALPREDICATELOCK *parentlock;
	bool		found,
				promote;

	promote = false;

	targettag = *reqtag;

	/* 迭代检查父项 */
	while (GetParentPredicateLockTag(&targettag, &nexttag))
	{
		targettag = nexttag;
		parentlock = (LOCALPREDICATELOCK *) hash_search(LocalPredicateLockHash,
														&targettag,
														HASH_ENTER,
														&found);
		if (!found)
		{
			parentlock->held = false;
			parentlock->childLocks = 1;
		}
		else
			parentlock->childLocks++;

		if (parentlock->childLocks >
			MaxPredicateChildLocks(&targettag))
		{
			/*
			 * 我们应当提升到这把父锁。不过继续检查它的
			 * 祖先，既为了把它们的子计数搞对，也为了
			 * 检查是否干脆提升到其中之一
			 * 它们。
			 */
			promotiontag = targettag;
			promote = true;
		}
	}

	if (promote)
	{
		/* 取得有资格提升的最粗祖先 */
		PredicateLockAcquire(&promotiontag);
		return true;
	}
	else
		return false;
}

/*
 * 释放一把锁时，递减所有祖先锁上的
 * 子计数。
 *
 * 仅在经由下列途径释放锁时才调用本函数：
 * DeleteChildTargetLocks（即因
 * 我们取得了它的父锁、可能是由于提升，而使某把锁变得多余）或当新的
 * MVCC 写锁使谓词锁不再必要时。在事务结束时释放锁
 * 去调用它没有意义，因为
 * 这些信息不再需要。
 */
static void
DecrementParentLocks(const PREDICATELOCKTARGETTAG *targettag)
{
	PREDICATELOCKTARGETTAG parenttag,
				nexttag;

	parenttag = *targettag;

	while (GetParentPredicateLockTag(&parenttag, &nexttag))
	{
		uint32		targettaghash;
		LOCALPREDICATELOCK *parentlock,
				   *rmlock PG_USED_FOR_ASSERTS_ONLY;

		parenttag = nexttag;
		targettaghash = PredicateLockTargetTagHashCode(&parenttag);
		parentlock = (LOCALPREDICATELOCK *)
			hash_search_with_hash_value(LocalPredicateLockHash,
										&parenttag, targettaghash,
										HASH_FIND, NULL);

		/*
		 * 父锁有很小的可能不存在于锁
		 * 表中。若我们过早移除了它，就会发生这种情况，因为一次
		 * 索引分裂使子引用计数偏离。
		 */
		if (parentlock == NULL)
			continue;

		parentlock->childLocks--;

		/*
		 * 类似情形下，父锁的引用计数可能
		 * 为零。只有当我们正持有那把锁时才会这样（否则我们
		 * 会已经移除该条目）。
		 */
		if (parentlock->childLocks < 0)
		{
			Assert(parentlock->held);
			parentlock->childLocks = 0;
		}

		if ((parentlock->childLocks == 0) && (!parentlock->held))
		{
			rmlock = (LOCALPREDICATELOCK *)
				hash_search_with_hash_value(LocalPredicateLockHash,
											&parenttag, targettaghash,
											HASH_REMOVE, NULL);
			Assert(rmlock == parentlock);
		}
	}
}

/*
 * 表明指定事务持有给定目标上的谓词锁。
 * 若锁已被持有，则没有效果。
 *
 * 这会更新锁表和 sxact 的锁列表，并在必要时创建
 * 锁目标，但*不*做任何与下列事项相关的事：
 * 粒度提升或本地锁表。见
 * PredicateLockAcquire。
 */
static void
CreatePredicateLock(const PREDICATELOCKTARGETTAG *targettag,
					uint32 targettaghash,
					SERIALIZABLEXACT *sxact)
{
	PREDICATELOCKTARGET *target;
	PREDICATELOCKTAG locktag;
	PREDICATELOCK *lock;
	LWLock	   *partitionLock;
	bool		found;

	partitionLock = PredicateLockHashPartitionLock(targettaghash);

	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/* 确保该目标已有代表。 */
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									targettag, targettaghash,
									HASH_ENTER_NULL, &found);
	if (!target)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase %s.", "max_pred_locks_per_transaction")));
	if (!found)
		dlist_init(&target->predicateLocks);

	/* 已经有了 sxact 和目标，确保它们被连接起来。 */
	locktag.myTarget = target;
	locktag.myXact = sxact;
	lock = (PREDICATELOCK *)
		hash_search_with_hash_value(PredicateLockHash, &locktag,
									PredicateLockHashCodeFromTargetHashCode(&locktag, targettaghash),
									HASH_ENTER_NULL, &found);
	if (!lock)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase %s.", "max_pred_locks_per_transaction")));

	if (!found)
	{
		dlist_push_tail(&target->predicateLocks, &lock->targetLink);
		dlist_push_tail(&sxact->predicateLocks, &lock->xactLink);
		lock->commitSeqNo = InvalidSerCommitSeqNo;
	}

	LWLockRelease(partitionLock);
	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * 为当前连接在指定目标上取得谓词锁，
 * 若尚未持有。这会更新本地锁表，
 * 并用它实现粒度提升。若有必要，它会把
 * 多把锁合并成一把更粗的锁，并释放
 * 被新锁覆盖的任何更细粒度的锁。
 */
static void
PredicateLockAcquire(const PREDICATELOCKTARGETTAG *targettag)
{
	uint32		targettaghash;
	bool		found;
	LOCALPREDICATELOCK *locallock;

	/* 我们是否已经有这把锁，或一把覆盖锁？ */
	if (PredicateLockExists(targettag))
		return;

	if (CoarserLockCovers(targettag))
		return;

	/* 同一散列和 LW 锁适用于锁目标与本地锁。 */
	targettaghash = PredicateLockTargetTagHashCode(targettag);

	/* 在本地表中取得锁 */
	locallock = (LOCALPREDICATELOCK *)
		hash_search_with_hash_value(LocalPredicateLockHash,
									targettag, targettaghash,
									HASH_ENTER, &found);
	locallock->held = true;
	if (!found)
		locallock->childLocks = 0;

	/* 实际创建该锁 */
	CreatePredicateLock(targettag, targettaghash, MySerializableXact);

	/*
	 * 锁已取得。检查是否应把它提升到
	 * 更粗的粒度，或者是否有更细粒度的锁需要
	 * 清理。
	 */
	if (CheckAndPromotePredicateLockRequest(targettag))
	{
		/*
		 * 锁请求被提升为一把更粗粒度的锁，并且那把
		 * 锁已取得。它会删除这把锁及其任何
		 * 子锁，因此我们做完了。
		 */
	}
	else
	{
		/* 清理任何更细粒度的锁 */
		if (GET_PREDICATELOCKTARGETTAG_TYPE(*targettag) != PREDLOCKTAG_TUPLE)
			DeleteChildTargetLocks(targettag);
	}
}


/*
 *		PredicateLockRelation
 *
 * 在关系级取得谓词锁。
 * 若不是完整的可串行化事务隔离级别，则跳过。
 * 若这是临时表，则跳过。
 * 清除本会话在该关系上的任何更细粒度谓词锁。
 */
void
PredicateLockRelation(Relation relation, Snapshot snapshot)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	SET_PREDICATELOCKTARGETTAG_RELATION(tag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	PredicateLockAcquire(&tag);
}

/*
 *		PredicateLockPage
 *
 * 在页级取得谓词锁。
 * 若不是完整的可串行化事务隔离级别，则跳过。
 * 若这是临时表，则跳过。
 * 若已有更粗的谓词锁覆盖此页，则跳过。
 * 清除本会话在该关系上的任何更细粒度谓词锁。
 */
void
PredicateLockPage(Relation relation, BlockNumber blkno, Snapshot snapshot)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	SET_PREDICATELOCKTARGETTAG_PAGE(tag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									blkno);
	PredicateLockAcquire(&tag);
}

/*
 *		PredicateLockTID
 *
 * 在元组级取得谓词锁。
 * 若不是完整的可串行化事务隔离级别，则跳过。
 * 若这是临时表，则跳过。
 */
void
PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
				 TransactionId tuple_xid)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	/*
	 * 若本事务写过它，则返回。
	 */
	if (relation->rd_index == NULL)
	{
		/* 若是我们写的，我们已经有写锁。 */
		if (TransactionIdIsCurrentTransactionId(tuple_xid))
			return;
	}

	/*
	 * 先对关系锁做快速但不绝对的测试。这
	 * 在关系*未*被锁定时绝不会导致返回，但会
	 * 在确实*有*关系
	 * 级锁时，偶尔让检查继续。
	 */
	SET_PREDICATELOCKTARGETTAG_RELATION(tag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	if (PredicateLockExists(&tag))
		return;

	SET_PREDICATELOCKTARGETTAG_TUPLE(tag,
									 relation->rd_locator.dbOid,
									 relation->rd_id,
									 ItemPointerGetBlockNumber(tid),
									 ItemPointerGetOffsetNumber(tid));
	PredicateLockAcquire(&tag);
}


/*
 *		DeleteLockTarget
 *
 * 移除谓词锁目标以及为它持有的任何锁。
 *
 * 调用者必须持有 SerializablePredicateListLock 以及
 * 该目标合适的散列分区锁。
 */
static void
DeleteLockTarget(PREDICATELOCKTARGET *target, uint32 targettaghash)
{
	dlist_mutable_iter iter;

	Assert(LWLockHeldByMeInMode(SerializablePredicateListLock,
								LW_EXCLUSIVE));
	Assert(LWLockHeldByMe(PredicateLockHashPartitionLock(targettaghash)));

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	dlist_foreach_modify(iter, &target->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, targetLink, iter.cur);
		bool		found;

		dlist_delete(&(predlock->xactLink));
		dlist_delete(&(predlock->targetLink));

		hash_search_with_hash_value
			(PredicateLockHash,
			 &predlock->tag,
			 PredicateLockHashCodeFromTargetHashCode(&predlock->tag,
													 targettaghash),
			 HASH_REMOVE, &found);
		Assert(found);
	}
	LWLockRelease(SerializableXactHashLock);

	/* 若可能，移除目标本身。 */
	RemoveTargetIfNoLongerUsed(target, targettaghash);
}


/*
 *		TransferPredicateLocksToNewTarget
 *
 * 移动或复制某锁目标的全部谓词锁，供
 * 索引页分裂/合并以及其他创建或替换
 * 锁目标的操作使用。若 'removeOld' 为 true，旧锁和该目标
 * 将被移除。
 *
 * 成功时返回 true；若共享内存不足以
 * 分配新目标或新锁，则返回 false。若设置了
 * removeOld（利用 PredicateLockTargetHash 中的暂存条目
 * 作为暂存空间），则保证总是成功。
 *
 * 警告："removeOld" 选项必须谨慎使用，
 * 因为本函数并不（实际上也不能）更新其他
 * 后端的 LocalPredicateLockHash。若我们只是添加新
 * 条目，这不是问题：本地锁表只用作
 * 提示，因此缺少正被持有的锁的条目是
 * 可以的。拥有不再被持有的锁的条目，如使用
 * "removeOld" 时可能发生的那样，一般不可以。我们只能在
 * 用一把覆盖它的更粗粒度锁替换某把锁时安全使用，
 * 或者在绝对确定将来没有人需要
 * 引用那把锁时。
 *
 * 调用者必须排他持有 SerializablePredicateListLock。
 */
static bool
TransferPredicateLocksToNewTarget(PREDICATELOCKTARGETTAG oldtargettag,
								  PREDICATELOCKTARGETTAG newtargettag,
								  bool removeOld)
{
	uint32		oldtargettaghash;
	LWLock	   *oldpartitionLock;
	PREDICATELOCKTARGET *oldtarget;
	uint32		newtargettaghash;
	LWLock	   *newpartitionLock;
	bool		found;
	bool		outOfShmem = false;

	Assert(LWLockHeldByMeInMode(SerializablePredicateListLock,
								LW_EXCLUSIVE));

	oldtargettaghash = PredicateLockTargetTagHashCode(&oldtargettag);
	newtargettaghash = PredicateLockTargetTagHashCode(&newtargettag);
	oldpartitionLock = PredicateLockHashPartitionLock(oldtargettaghash);
	newpartitionLock = PredicateLockHashPartitionLock(newtargettaghash);

	if (removeOld)
	{
		/*
		 * 移除哑条目以给我们暂存空间，这样我们就知道将能够
		 * 创建新的锁目标。
		 */
		RemoveScratchTarget(false);
	}

	/*
	 * 我们必须按升序取得分区锁，以避免
	 * 死锁。若新旧分区相同，我们只应请求该
	 * 锁一次。
	 */
	if (oldpartitionLock < newpartitionLock)
	{
		LWLockAcquire(oldpartitionLock,
					  (removeOld ? LW_EXCLUSIVE : LW_SHARED));
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);
	}
	else if (oldpartitionLock > newpartitionLock)
	{
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);
		LWLockAcquire(oldpartitionLock,
					  (removeOld ? LW_EXCLUSIVE : LW_SHARED));
	}
	else
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);

	/*
	 * 查找旧目标。若未找到，那也没问题；没有谓词锁
	 * 受影响，因此我们可以清理并返回。若它确实存在，
	 * 遍历它的谓词锁列表，把它们移动或复制到新
	 * 目标。
	 */
	oldtarget = hash_search_with_hash_value(PredicateLockTargetHash,
											&oldtargettag,
											oldtargettaghash,
											HASH_FIND, NULL);

	if (oldtarget)
	{
		PREDICATELOCKTARGET *newtarget;
		PREDICATELOCKTAG newpredlocktag;
		dlist_mutable_iter iter;

		newtarget = hash_search_with_hash_value(PredicateLockTargetHash,
												&newtargettag,
												newtargettaghash,
												HASH_ENTER_NULL, &found);

		if (!newtarget)
		{
			/* 因共享内存不足而分配失败 */
			outOfShmem = true;
			goto exit;
		}

		/* 若我们创建了新条目，则初始化它 */
		if (!found)
			dlist_init(&newtarget->predicateLocks);

		newpredlocktag.myTarget = newtarget;

		/*
		 * 遍历旧目标上的全部锁，用
		 * 新目标上的锁替换它们。
		 */
		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		dlist_foreach_modify(iter, &oldtarget->predicateLocks)
		{
			PREDICATELOCK *oldpredlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo = oldpredlock->commitSeqNo;

			newpredlocktag.myXact = oldpredlock->tag.myXact;

			if (removeOld)
			{
				dlist_delete(&(oldpredlock->xactLink));
				dlist_delete(&(oldpredlock->targetLink));

				hash_search_with_hash_value
					(PredicateLockHash,
					 &oldpredlock->tag,
					 PredicateLockHashCodeFromTargetHashCode(&oldpredlock->tag,
															 oldtargettaghash),
					 HASH_REMOVE, &found);
				Assert(found);
			}

			newpredlock = (PREDICATELOCK *)
				hash_search_with_hash_value(PredicateLockHash,
											&newpredlocktag,
											PredicateLockHashCodeFromTargetHashCode(&newpredlocktag,
																					newtargettaghash),
											HASH_ENTER_NULL,
											&found);
			if (!newpredlock)
			{
				/* 共享内存耗尽。撤销到目前为止所做的事。 */
				LWLockRelease(SerializableXactHashLock);
				DeleteLockTarget(newtarget, newtargettaghash);
				outOfShmem = true;
				goto exit;
			}
			if (!found)
			{
				dlist_push_tail(&(newtarget->predicateLocks),
								&(newpredlock->targetLink));
				dlist_push_tail(&(newpredlocktag.myXact->predicateLocks),
								&(newpredlock->xactLink));
				newpredlock->commitSeqNo = oldCommitSeqNo;
			}
			else
			{
				if (newpredlock->commitSeqNo < oldCommitSeqNo)
					newpredlock->commitSeqNo = oldCommitSeqNo;
			}

			Assert(newpredlock->commitSeqNo != 0);
			Assert((newpredlock->commitSeqNo == InvalidSerCommitSeqNo)
				   || (newpredlock->tag.myXact == OldCommittedSxact));
		}
		LWLockRelease(SerializableXactHashLock);

		if (removeOld)
		{
			Assert(dlist_is_empty(&oldtarget->predicateLocks));
			RemoveTargetIfNoLongerUsed(oldtarget, oldtargettaghash);
		}
	}


exit:
	/* 按取得时的相反顺序释放分区锁。 */
	if (oldpartitionLock < newpartitionLock)
	{
		LWLockRelease(newpartitionLock);
		LWLockRelease(oldpartitionLock);
	}
	else if (oldpartitionLock > newpartitionLock)
	{
		LWLockRelease(oldpartitionLock);
		LWLockRelease(newpartitionLock);
	}
	else
		LWLockRelease(newpartitionLock);

	if (removeOld)
	{
		/* 若我们是在移动锁，就不应当耗尽内存 */
		Assert(!outOfShmem);

		/* 把暂存条目放回去 */
		RestoreScratchTarget(false);
	}

	return !outOfShmem;
}

/*
 * 从指定关系上丢弃任何粒度的全部谓词锁，
 * 该关系可以是堆关系或索引关系。若 'transfer' 为 true，
 * 则为在其上持有任何锁的事务，在堆上取得一把关系锁，
 * 这些锁位于指定关系上。
 *
 * 这需要抓取许多轻量锁，并扫描整个锁
 * 目标表以寻找匹配。这使它比大多数
 * 谓词锁管理函数更贵，但它只会被 DDL
 * 一类本来就昂贵的命令调用，并且当
 * 没有活动的可串行化事务或该关系是临时的时会快速返回。
 *
 * 我们不使用 TransferPredicateLocksToNewTarget 函数，因为它
 * 会自己取得所涉及的两个目标的分区上的锁，
 * 而我们将已经持有全部分区锁。
 *
 * 我们不能从这里抛出错误，因为调用可能来自一个
 * 并非可串行化的事务。
 *
 * 注意：目前只在 transfer 设为 true 时调用，但那可能
 * 会改变。若我们决定在执行了 DROP TABLE 的事务提交时清理该表上的锁，
 * 则 false 这一条件会有用。
 */
static void
DropAllPredicateLocksFromTable(Relation relation, bool transfer)
{
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCKTARGET *oldtarget;
	PREDICATELOCKTARGET *heaptarget;
	Oid			dbId;
	Oid			relId;
	Oid			heapId;
	int			i;
	bool		isIndex;
	bool		found;
	uint32		heaptargettaghash;

	/*
	 * 若没有正在运行的可串行化事务，则迅速退出。
	 * 不加锁做此检查是安全的，因为调用者
	 * 持有该关系上的 ACCESS EXCLUSIVE 锁。在持有该锁期间，不能取得这里
	 * 会要紧的新锁。
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!PredicateLockingNeededForRelation(relation))
		return;

	dbId = relation->rd_locator.dbOid;
	relId = relation->rd_id;
	if (relation->rd_index == NULL)
	{
		isIndex = false;
		heapId = relId;
	}
	else
	{
		isIndex = true;
		heapId = relation->rd_index->indrelid;
	}
	Assert(heapId != InvalidOid);
	Assert(transfer || !isIndex);	/* 索引 OID 只在与下列值一起时才有意义：
									 * transfer */

	/* 第一次需要时取出，然后保留。 */
	heaptargettaghash = 0;
	heaptarget = NULL;

	/* 取得所有锁分区上的锁 */
	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_EXCLUSIVE);
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * 移除哑条目以给我们暂存空间，这样我们就知道将能够
	 * 创建新的锁目标。
	 */
	if (transfer)
		RemoveScratchTarget(true);

	/* 扫描目标映射 */
	hash_seq_init(&seqstat, PredicateLockTargetHash);

	while ((oldtarget = (PREDICATELOCKTARGET *) hash_seq_search(&seqstat)))
	{
		dlist_mutable_iter iter;

		/*
		 * 检查这是否是需要关注的目标。
		 */
		if (GET_PREDICATELOCKTARGETTAG_RELATION(oldtarget->tag) != relId)
			continue;			/* 错误的关系 id */
		if (GET_PREDICATELOCKTARGETTAG_DB(oldtarget->tag) != dbId)
			continue;			/* 错误的数据库 id */
		if (transfer && !isIndex
			&& GET_PREDICATELOCKTARGETTAG_TYPE(oldtarget->tag) == PREDLOCKTAG_RELATION)
			continue;			/* 已经是正确的锁 */

		/*
		 * 若我们走到这里，就有工作要做。我们确保堆
		 * 关系锁存在，然后遍历找到的旧目标上的谓词锁列表，
		 * 把找到的旧目标上的全部锁移到堆关系锁，
		 * ——除非它们已经持有那把锁。
		 */

		/*
		 * 首先确保我们有堆关系目标。我们只需要
		 * 做这一次。
		 */
		if (transfer && heaptarget == NULL)
		{
			PREDICATELOCKTARGETTAG heaptargettag;

			SET_PREDICATELOCKTARGETTAG_RELATION(heaptargettag, dbId, heapId);
			heaptargettaghash = PredicateLockTargetTagHashCode(&heaptargettag);
			heaptarget = hash_search_with_hash_value(PredicateLockTargetHash,
													 &heaptargettag,
													 heaptargettaghash,
													 HASH_ENTER, &found);
			if (!found)
				dlist_init(&heaptarget->predicateLocks);
		}

		/*
		 * 遍历旧目标上的全部锁，用
		 * 新目标上的锁替换它们。
		 */
		dlist_foreach_modify(iter, &oldtarget->predicateLocks)
		{
			PREDICATELOCK *oldpredlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo;
			SERIALIZABLEXACT *oldXact;

			/*
			 * 先移除旧锁。这避免了耗尽
			 * 散列表锁结构条目的可能。
			 */
			oldCommitSeqNo = oldpredlock->commitSeqNo;
			oldXact = oldpredlock->tag.myXact;

			dlist_delete(&(oldpredlock->xactLink));

			/*
			 * 不必从 oldtarget 列表逐条删除，我们反正要移除
			 * 整个目标。
			 */
			hash_search(PredicateLockHash,
						&oldpredlock->tag,
						HASH_REMOVE, &found);
			Assert(found);

			if (transfer)
			{
				PREDICATELOCKTAG newpredlocktag;

				newpredlocktag.myTarget = heaptarget;
				newpredlocktag.myXact = oldXact;
				newpredlock = (PREDICATELOCK *)
					hash_search_with_hash_value(PredicateLockHash,
												&newpredlocktag,
												PredicateLockHashCodeFromTargetHashCode(&newpredlocktag,
																						heaptargettaghash),
												HASH_ENTER,
												&found);
				if (!found)
				{
					dlist_push_tail(&(heaptarget->predicateLocks),
									&(newpredlock->targetLink));
					dlist_push_tail(&(newpredlocktag.myXact->predicateLocks),
									&(newpredlock->xactLink));
					newpredlock->commitSeqNo = oldCommitSeqNo;
				}
				else
				{
					if (newpredlock->commitSeqNo < oldCommitSeqNo)
						newpredlock->commitSeqNo = oldCommitSeqNo;
				}

				Assert(newpredlock->commitSeqNo != 0);
				Assert((newpredlock->commitSeqNo == InvalidSerCommitSeqNo)
					   || (newpredlock->tag.myXact == OldCommittedSxact));
			}
		}

		hash_search(PredicateLockTargetHash, &oldtarget->tag, HASH_REMOVE,
					&found);
		Assert(found);
	}

	/* 把暂存条目放回去 */
	if (transfer)
		RestoreScratchTarget(true);

	/* 按相反顺序释放锁 */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * TransferPredicateLocksToHeapRelation
 *		对所有事务，把给定
 *		关系的全部谓词锁转移到堆上的一把关系锁。
 */
void
TransferPredicateLocksToHeapRelation(Relation relation)
{
	DropAllPredicateLocksFromTable(relation, true);
}


/*
 *		PredicateLockPageSplit
 *
 * 把旧页上的任何谓词锁复制到新页。
 * 若这是临时表或 toast 表，则跳过。
 *
 * 注意：页分裂（或溢出）影响所有可串行化事务，
 * 即便它发生在另一种事务隔离级别的上下文中。
 *
 * 注意：这目前使锁的本地副本缺少
 * 共享内存中那把新锁的信息。这可能造成
 * 问题，若在被锁定的页上发生足够多的页分裂，而那些进程
 * 持有这些锁却没有进来并注意到。
 */
void
PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
					   BlockNumber newblkno)
{
	PREDICATELOCKTARGETTAG oldtargettag;
	PREDICATELOCKTARGETTAG newtargettag;
	bool		success;

	/*
	 * 若没有正在运行的可串行化事务，则迅速退出。
	 *
	 * 不做任何额外加锁来做此检查是安全的。即便
	 * 一个可串行化事务并发启动，我们也知道它不能取得
	 * 正在分裂的页上的任何 SIREAD 锁，因为调用者持有
	 * 相关联的缓冲区页锁。内存重排不是问题；
	 * LWLock 取得操作中的内存屏障保证这次读
	 * 发生在持有缓冲区页锁期间。
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!PredicateLockingNeededForRelation(relation))
		return;

	Assert(oldblkno != newblkno);
	Assert(BlockNumberIsValid(oldblkno));
	Assert(BlockNumberIsValid(newblkno));

	SET_PREDICATELOCKTARGETTAG_PAGE(oldtargettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									oldblkno);
	SET_PREDICATELOCKTARGETTAG_PAGE(newtargettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									newblkno);

	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);

	/*
	 * 尝试把锁复制到新页的标签上，必要时创建该标签
	 * 必要。
	 */
	success = TransferPredicateLocksToNewTarget(oldtargettag,
												newtargettag,
												false);

	if (!success)
	{
		/*
		 * 没有更多谓词锁条目可用。这里不允许
		 * 失败，因此把页锁提升为关系锁。
		 */

		/* 取得父关系锁的锁标签 */
		success = GetParentPredicateLockTag(&oldtargettag,
											&newtargettag);
		Assert(success);

		/*
		 * 把锁移到父项。这不应当失败。
		 *
		 * 注意，这里我们在移除其他后端持有的锁，
		 * 这可能导致它们的本地锁散列表不一致。
		 * 这是可以的，因为我们用一把覆盖
		 * 旧锁的锁替换了它。
		 */
		success = TransferPredicateLocksToNewTarget(oldtargettag,
													newtargettag,
													true);
		Assert(success);
	}

	LWLockRelease(SerializablePredicateListLock);
}

/*
 *		PredicateLockPageCombine
 *
 * 合并两个已有页上的谓词锁。
 * 若这是临时表或 toast 表，则跳过。
 *
 * 注意：页合并影响所有可串行化事务，即便它
 * 发生在另一种事务隔离级别的上下文中。
 */
void
PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
						 BlockNumber newblkno)
{
	/*
	 * 页合并与页分裂的不同在于，我们应当能够
	 * 在把锁转移到新页之后移除旧页上的锁，
	 * 而不是把它们复制一份。不过，因为我们不能编辑其他
	 * 后端的本地锁表，移除旧锁会给他们留下
	 * LocalPredicateLockHash 中的一条条目，对应一把他们并未
	 * 持有的锁，这不可接受。因此我们最终不得不做同样的
	 * 工作，像页分裂那样：在新页上取得锁，并让
	 * 旧页也保持锁定。这可能导致一些假阳性，但应当
	 * 在实践中很少见。
	 */
	PredicateLockPageSplit(relation, oldblkno, newblkno);
}

/*
 * 遍历进行中的可串行化事务列表，并找到新的
 * xmin。
 */
static void
SetNewSxactGlobalXmin(void)
{
	dlist_iter	iter;

	Assert(LWLockHeldByMe(SerializableXactHashLock));

	PredXact->SxactGlobalXmin = InvalidTransactionId;
	PredXact->SxactGlobalXminCount = 0;

	dlist_foreach(iter, &PredXact->activeList)
	{
		SERIALIZABLEXACT *sxact =
			dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

		if (!SxactIsRolledBack(sxact)
			&& !SxactIsCommitted(sxact)
			&& sxact != OldCommittedSxact)
		{
			Assert(sxact->xmin != InvalidTransactionId);
			if (!TransactionIdIsValid(PredXact->SxactGlobalXmin)
				|| TransactionIdPrecedes(sxact->xmin,
										 PredXact->SxactGlobalXmin))
			{
				PredXact->SxactGlobalXmin = sxact->xmin;
				PredXact->SxactGlobalXminCount = 1;
			}
			else if (TransactionIdEquals(sxact->xmin,
										 PredXact->SxactGlobalXmin))
				PredXact->SxactGlobalXminCount++;
		}
	}

	SerialSetActiveSerXmin(PredXact->SxactGlobalXmin);
}

/*
 *		ReleasePredicateLocks
 *
 * 根据当前事务的完成来释放谓词锁，
 * 无论是提交还是回滚。也可以在只读
 * 事务变得不可能再成为
 * 危险结构的一部分时，为它调用。
 *
 * 除非这是可串行化事务，否则我们什么也不做。
 *
 * 本方法必须确保共享内存散列表以
 * 某种相对及时的方式得到清理。
 *
 * 若本事务正在提交，并且持有任何谓词锁，
 * 就必须把它加入一份已完成、但仍
 * 持有锁的可串行化事务列表。
 *
 * 若 isReadOnlySafe 为 true，则谓词锁会在
 * 事务结束之前被释放，因为已判定 MySerializableXact
 * 为 RO_SAFE。非并行模式下我们可以完全释放它，但在
 * 并行模式下我们部分释放 SERIALIZABLEXACT，并把它保留
 * 到事务结束，以便每个后端清除自己的
 * MySerializableXact 变量，并在它自己的
 * 时机。
 */
void
ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe)
{
	bool		partiallyReleasing = false;
	bool		needToClear;
	SERIALIZABLEXACT *roXact;
	dlist_mutable_iter iter;

	/*
	 * 这里不能信任 XactReadOnly，因为一个以
	 * READ WRITE 开始的事务后来可能显示为 READ ONLY，例如在
	 * 子事务之内。若事务
	 * 提交时没有写，我们想把它标为 READ ONLY，以便事实上的只读事务得到
	 * 某些只读优化的好处，因此我们将用这个局部变量来
	 * 把一些清理逻辑做对，这些逻辑基于该事务是否
	 * 在顶层被声明为 READ ONLY。
	 */
	bool		topLevelIsDeclaredReadOnly;

	/* 我们不能既正在提交，又因 RO_SAFE 而提前释放。 */
	Assert(!(isCommit && isReadOnlySafe));

	/* 我们是否处于事务的末尾，也就是提交或中止？ */
	if (!isReadOnlySafe)
	{
		/*
		 * 并行工作进程不得在自己事务的末尾
		 * 释放谓词锁。领导会在自己事务的末尾
		 * 做这件事。
		 */
		if (IsParallelWorker())
		{
			ReleasePredicateLocksLocal();
			return;
		}

		/*
		 * 当并行查询中的领导到达
		 * 事务末尾时，它已经等待所有工作进程退出。
		 */
		Assert(!ParallelContextActive());

		/*
		 * 若并行查询中的领导先前存放了一个部分
		 * 释放的 SERIALIZABLEXACT，留到事务末尾做最终清理
		 * （因为工作进程当时可能仍在访问它），那么现在
		 * 到了把它恢复回来的时候。
		 */
		if (SavedSerializableXact != InvalidSerializableXact)
		{
			Assert(MySerializableXact == InvalidSerializableXact);
			MySerializableXact = SavedSerializableXact;
			SavedSerializableXact = InvalidSerializableXact;
			Assert(SxactIsPartiallyReleased(MySerializableXact));
		}
	}

	if (MySerializableXact == InvalidSerializableXact)
	{
		Assert(LocalPredicateLockHash == NULL);
		return;
	}

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * 若事务正在提交，但已经被部分释放
	 * 过了，则把它当作回滚处理。它已被标为已回滚。
	 */
	if (isCommit && SxactIsPartiallyReleased(MySerializableXact))
		isCommit = false;

	/*
	 * 若因为我们发现
	 * SXACT_FLAG_RO_SAFE 标志已置位而在事务中途被调用，则我们将部分释放
	 * 它（也就是释放谓词锁和冲突，但不释放
	 * SERIALIZABLEXACT 本身），前提是我们是第一个注意到的后端。
	 */
	if (isReadOnlySafe && IsInParallelMode())
	{
		/*
		 * 领导需要存放一个指向它的指针，以便能够
		 * 在事务结束时完全释放它。
		 */
		if (!IsParallelWorker())
			SavedSerializableXact = MySerializableXact;

		/*
		 * 第一个到达此条件的后端将部分释放
		 * SERIALIZABLEXACT。其余后端只清除自己的
		 * 后端本地状态，以便在本事务的其余部分
		 * 停止做 SSI 检查。
		 */
		if (SxactIsPartiallyReleased(MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			ReleasePredicateLocksLocal();
			return;
		}
		else
		{
			MySerializableXact->flags |= SXACT_FLAG_PARTIALLY_RELEASED;
			partiallyReleasing = true;
			/* ... 并继续执行下面的部分释放。 */
		}
	}
	Assert(!isCommit || SxactIsPrepared(MySerializableXact));
	Assert(!isCommit || !SxactIsDoomed(MySerializableXact));
	Assert(!SxactIsCommitted(MySerializableXact));
	Assert(SxactIsPartiallyReleased(MySerializableXact)
		   || !SxactIsRolledBack(MySerializableXact));

	/* 在 COMMIT/ROLLBACK PREPARED 期间，它可能不是可串行化的 */
	Assert(MySerializableXact->pid == 0 || IsolationIsSerializable());

	/* 我们最好还没有位于清理列表上。 */
	Assert(!SxactIsOnFinishedList(MySerializableXact));

	topLevelIsDeclaredReadOnly = SxactIsReadOnly(MySerializableXact);

	/*
	 * 这里不持有 XidGenLock，假定 TransactionId 是
	 * 原子的！
	 *
	 * 若这个值正在变化，我们不太在意取到的是
	 * 旧值还是新值——它只用来确定
	 * SxactGlobalXmin 必须前进多远，本事务才能被完全
	 * 清理掉。最坏的情况是，我们要再等一个
	 * 事务完成，才释放一些 RAM；可见
	 * 行为的正确性不受影响。
	 */
	MySerializableXact->finishedBefore = XidFromFullTransactionId(ShmemVariableCache->nextXid);

	/*
	 * 若这不是提交，则它要么是回滚，要么是被标为
	 * SXACT_FLAG_RO_SAFE 的只读事务，我们可以立即清除自己的锁。
	 */
	if (isCommit)
	{
		MySerializableXact->flags |= SXACT_FLAG_COMMITTED;
		MySerializableXact->commitSeqNo = ++(PredXact->LastSxactCommitSeqNo);
		/* 识别隐式只读事务（没有写就提交）。 */
		if (!MyXactDidWrite)
			MySerializableXact->flags |= SXACT_FLAG_READ_ONLY;
	}
	else
	{
		/*
		 * DOOMED 标志表示我们打算回滚此
		 * 事务，因此它不应当给与它冲突的
		 * 其他事务造成序列化失败。注意，此标志可能
		 * 已经置位，如果另一个后端已把此事务标记为即将
		 * 中止。
		 *
		 * ROLLED_BACK 标志进一步表明已经调用了 ReleasePredicateLocks，
		 * 因此该 SerializableXact 有资格进入
		 * 清理。这意味着在计算下列值时不应考虑它：
		 * SxactGlobalXmin。
		 */
		MySerializableXact->flags |= SXACT_FLAG_DOOMED;
		MySerializableXact->flags |= SXACT_FLAG_ROLLED_BACK;

		/*
		 * 若该事务先前已经预备，但现在失败了，原因是
		 * 一次 ROLLBACK PREPARED，或（希望极少见）预备之后的错误，
		 * 则清除 prepared 标志。这简化了冲突方面的
		 * 检查。
		 */
		MySerializableXact->flags &= ~SXACT_FLAG_PREPARED;
	}

	if (!topLevelIsDeclaredReadOnly)
	{
		Assert(PredXact->WritableSxactCount > 0);
		if (--(PredXact->WritableSxactCount) == 0)
		{
			/*
			 * 为所有已提交事务释放谓词锁以及进入方向的读写依赖（rw-conflict）。
			 * 不再有任何事务可能
			 * 与这些锁冲突，新事务也没有机会与之
			 * 重叠。类似地，已有的冲突入不能形成枢纽，
			 * 而任何本来能够补全危险
			 * 结构的冲突入，早就已经导致了回滚，因此任何
			 * 剩余的冲突入都必定是良性的。
			 */
			PredXact->CanPartialClearThrough = PredXact->LastSxactCommitSeqNo;
		}
	}
	else
	{
		/*
		 * 只读事务：清除那些可能
		 * 使我们变得不安全的事务列表。注意，迭代时我们使用 'inLink'，
		 * 这与读/写事务使用 'outLink' 相对。
		 */
		dlist_foreach_modify(iter, &MySerializableXact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, inLink, iter.cur);

			Assert(!SxactIsReadOnly(possibleUnsafeConflict->sxactOut));
			Assert(MySerializableXact == possibleUnsafeConflict->sxactIn);

			ReleaseRWConflict(possibleUnsafeConflict);
		}
	}

	/* 检查指向旧的已提交事务的冲突出。 */
	if (isCommit
		&& !SxactIsReadOnly(MySerializableXact)
		&& SxactHasSummaryConflictOut(MySerializableXact))
	{
		/*
		 * 我们不知道与哪一个旧的已提交事务发生了冲突，
		 * 因此这里保守地使用 FirstNormalSerCommitSeqNo
		 */
		MySerializableXact->SeqNo.earliestOutConflictCommit =
			FirstNormalSerCommitSeqNo;
		MySerializableXact->flags |= SXACT_FLAG_CONFLICT_OUT;
	}

	/*
	 * 释放指向已提交事务的全部 outConflicts。若我们正在回滚，
	 * 则把它们全部清除。若其中有任何指向
	 * 先前已经提交的事务，则置位 SXACT_FLAG_CONFLICT_OUT。
	 */
	dlist_foreach_modify(iter, &MySerializableXact->outConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, outLink, iter.cur);

		if (isCommit
			&& !SxactIsReadOnly(MySerializableXact)
			&& SxactIsCommitted(conflict->sxactIn))
		{
			if ((MySerializableXact->flags & SXACT_FLAG_CONFLICT_OUT) == 0
				|| conflict->sxactIn->prepareSeqNo < MySerializableXact->SeqNo.earliestOutConflictCommit)
				MySerializableXact->SeqNo.earliestOutConflictCommit = conflict->sxactIn->prepareSeqNo;
			MySerializableXact->flags |= SXACT_FLAG_CONFLICT_OUT;
		}

		if (!isCommit
			|| SxactIsCommitted(conflict->sxactIn)
			|| (conflict->sxactIn->SeqNo.lastCommitBeforeSnapshot >= PredXact->LastSxactCommitSeqNo))
			ReleaseRWConflict(conflict);
	}

	/*
	 * 释放来自已提交事务和只读事务的全部 inConflicts。若
	 * 我们正在回滚，则把它们全部清除。
	 */
	dlist_foreach_modify(iter, &MySerializableXact->inConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		if (!isCommit
			|| SxactIsCommitted(conflict->sxactOut)
			|| SxactIsReadOnly(conflict->sxactOut))
			ReleaseRWConflict(conflict);
	}

	if (!topLevelIsDeclaredReadOnly)
	{
		/*
		 * 把自己从并发的那些
		 * READ ONLY 事务的可能冲突列表中移除；若我们有
		 * 冲突出，就把它们标为不安全。若有正在等待的 DEFERRABLE 事务，则唤醒它们，
		 * 当已经知道它们是安全的或不安全的时候。
		 */
		dlist_foreach_modify(iter, &MySerializableXact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, outLink, iter.cur);

			roXact = possibleUnsafeConflict->sxactIn;
			Assert(MySerializableXact == possibleUnsafeConflict->sxactOut);
			Assert(SxactIsReadOnly(roXact));

			/* 必要时把它标为已冲突。 */
			if (isCommit
				&& MyXactDidWrite
				&& SxactHasConflictOut(MySerializableXact)
				&& (MySerializableXact->SeqNo.earliestOutConflictCommit
					<= roXact->SeqNo.lastCommitBeforeSnapshot))
			{
				/*
				 * 这会释放 possibleUnsafeConflict（以及 roXact 的所有其他
				 * 可能冲突）
				 */
				FlagSxactUnsafe(roXact);
			}
			else
			{
				ReleaseRWConflict(possibleUnsafeConflict);

				/*
				 * 若我们是最后一个可能的冲突，就把它标为安全。该
				 * 事务现在可以安全地释放自己的谓词锁（但
				 * 必须由那个事务自己的后端来做）。
				 */
				if (dlist_is_empty(&roXact->possibleUnsafeConflicts))
					roXact->flags |= SXACT_FLAG_RO_SAFE;
			}

			/*
			 * 若一个正在等待的 DEFERRABLE 事务，我们现在已经知道
			 * 它要么安全、要么已经冲突，就唤醒它的进程。
			 */
			if (SxactIsDeferrableWaiting(roXact) &&
				(SxactIsROUnsafe(roXact) || SxactIsROSafe(roXact)))
				ProcSendSignal(roXact->pgprocno);
		}
	}

	/*
	 * 检查是否到了清理旧事务的时候。这只能
	 * 在可串行化事务之中、拥有最旧 xmin 的最后一个可串行化事务
	 * 完成时进行。然后我们找出 "新的最旧"
	 * xmin，并清除那些在本事务
	 * 启动之前就已经结束的事务。
	 *
	 * 对只读事务中的并行查询，它可能运行两次。我们
	 * 只在第一次调用时释放该引用。
	 */
	needToClear = false;
	if ((partiallyReleasing ||
		 !SxactIsPartiallyReleased(MySerializableXact)) &&
		TransactionIdEquals(MySerializableXact->xmin,
							PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount > 0);
		if (--(PredXact->SxactGlobalXminCount) == 0)
		{
			SetNewSxactGlobalXmin();
			needToClear = true;
		}
	}

	LWLockRelease(SerializableXactHashLock);

	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);

	/* 把它加入稍后要检查清理的事务列表。 */
	if (isCommit)
		dlist_push_tail(FinishedSerializableTransactions,
						&MySerializableXact->finishedLink);

	/*
	 * 若我们在并行模式下释放一个 RO_SAFE 事务，则只会
	 * 部分释放它。这是必要的，因为其他后端可能仍持有
	 * 对它的引用。领导会在工作进程停止运行之后，
	 * 于事务结束时释放 SERIALIZABLEXACT 本身。
	 */
	if (!isCommit)
		ReleaseOneSerializableXact(MySerializableXact,
								   isReadOnlySafe && IsInParallelMode(),
								   false);

	LWLockRelease(SerializableFinishedListLock);

	if (needToClear)
		ClearOldPredicateLocks();

	ReleasePredicateLocksLocal();
}

static void
ReleasePredicateLocksLocal(void)
{
	MySerializableXact = InvalidSerializableXact;
	MyXactDidWrite = false;

	/* 删除按事务的锁表 */
	if (LocalPredicateLockHash != NULL)
	{
		hash_destroy(LocalPredicateLockHash);
		LocalPredicateLockHash = NULL;
	}
}

/*
 * 清除那些属于已提交事务、并且对任何进行中的事务
 * 都不再有意义的旧谓词锁。
 */
static void
ClearOldPredicateLocks(void)
{
	dlist_mutable_iter iter;

	/*
	 * 遍历已结束的事务。它们按提交顺序排列，因此我们可以
	 * 一发现某个仍然有意义的事务就停下来。
	 */
	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	dlist_foreach_modify(iter, FinishedSerializableTransactions)
	{
		SERIALIZABLEXACT *finishedSxact =
			dlist_container(SERIALIZABLEXACT, finishedLink, iter.cur);

		if (!TransactionIdIsValid(PredXact->SxactGlobalXmin)
			|| TransactionIdPrecedesOrEquals(finishedSxact->finishedBefore,
											 PredXact->SxactGlobalXmin))
		{
			/*
			 * 本事务在任何进行中的事务
			 * 取得其快照之前就已经提交。它不再有意义。
			 */
			LWLockRelease(SerializableXactHashLock);
			dlist_delete_thoroughly(&finishedSxact->finishedLink);
			ReleaseOneSerializableXact(finishedSxact, false, false);
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
		else if (finishedSxact->commitSeqNo > PredXact->HavePartialClearedThrough
				 && finishedSxact->commitSeqNo <= PredXact->CanPartialClearThrough)
		{
			/*
			 * 任何在此
			 * 事务提交之前就取得了快照的活动事务都是只读的，因此我们可以清除它的一部分
			 * 状态。
			 */
			LWLockRelease(SerializableXactHashLock);

			if (SxactIsReadOnly(finishedSxact))
			{
				/* 只读事务可以整个移除 */
				dlist_delete_thoroughly(&(finishedSxact->finishedLink));
				ReleaseOneSerializableXact(finishedSxact, false, false);
			}
			else
			{
				/*
				 * 读写事务只能部分清除。我们
				 * 需要保留 SERIALIZABLEXACT，但可以释放
				 * SIREAD 锁以及冲突入。
				 */
				ReleaseOneSerializableXact(finishedSxact, true, false);
			}

			PredXact->HavePartialClearedThrough = finishedSxact->commitSeqNo;
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
		else
		{
			/* 仍然有意义。 */
			break;
		}
	}
	LWLockRelease(SerializableXactHashLock);

	/*
	 * 遍历哑事务上、用于汇总数据的谓词锁。
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	dlist_foreach_modify(iter, &OldCommittedSxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);
		bool		canDoPartialCleanup;

		LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		Assert(predlock->commitSeqNo != 0);
		Assert(predlock->commitSeqNo != InvalidSerCommitSeqNo);
		canDoPartialCleanup = (predlock->commitSeqNo <= PredXact->CanPartialClearThrough);
		LWLockRelease(SerializableXactHashLock);

		/*
		 * 若这把锁原本属于一个足够旧的事务，我们
		 * 就可以释放它。
		 */
		if (canDoPartialCleanup)
		{
			PREDICATELOCKTAG tag;
			PREDICATELOCKTARGET *target;
			PREDICATELOCKTARGETTAG targettag;
			uint32		targettaghash;
			LWLock	   *partitionLock;

			tag = predlock->tag;
			target = tag.myTarget;
			targettag = target->tag;
			targettaghash = PredicateLockTargetTagHashCode(&targettag);
			partitionLock = PredicateLockHashPartitionLock(targettaghash);

			LWLockAcquire(partitionLock, LW_EXCLUSIVE);

			dlist_delete(&(predlock->targetLink));
			dlist_delete(&(predlock->xactLink));

			hash_search_with_hash_value(PredicateLockHash, &tag,
										PredicateLockHashCodeFromTargetHashCode(&tag,
																				targettaghash),
										HASH_REMOVE, NULL);
			RemoveTargetIfNoLongerUsed(target, targettaghash);

			LWLockRelease(partitionLock);
		}
	}

	LWLockRelease(SerializablePredicateListLock);
	LWLockRelease(SerializableFinishedListLock);
}

/*
 * 这是从任何谓词
 * 加锁散列表中删除内容的正常方式。给定一个我们知道可以删除的事务：
 * 删除该事务持有的全部谓词锁，以及现在已没有任何锁引用的那些谓词
 * 锁目标；删除该事务的全部冲突；
 * 删除该事务的全部 xid 值；然后
 * 删除该事务。
 *
 * 当 partial 标志置位时，我们可以释放全部谓词锁以及
 * 冲突入信息——我们已经确定，不再有任何
 * 重叠的读写事务，会使得本事务仍然
 * 要紧——但保留事务条目本身，以及任何 outConflicts。
 *
 * 当 summarize 标志置位时，sxact 数据的空间已经不够，
 * 必须汇总到 SLRU。谓词锁被转移到一个
 * 表示 "旧" 的哑事务；同一目标上的重复锁
 * 合并成一把锁，commitSeqNo 取各冲突锁中 "最新" 的
 * 那一个。
 */
static void
ReleaseOneSerializableXact(SERIALIZABLEXACT *sxact, bool partial,
						   bool summarize)
{
	SERIALIZABLEXIDTAG sxidtag;
	dlist_mutable_iter iter;

	Assert(sxact != NULL);
	Assert(SxactIsRolledBack(sxact) || SxactIsCommitted(sxact));
	Assert(partial || !SxactIsOnFinishedList(sxact));
	Assert(LWLockHeldByMe(SerializableFinishedListLock));

	/*
	 * 首先释放本 xact 持有的全部谓词锁（或者，若 summarize 为 true，把它们转移
	 * 到 OldCommittedSxact）
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);
	dlist_foreach_modify(iter, &sxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);
		PREDICATELOCKTAG tag;
		PREDICATELOCKTARGET *target;
		PREDICATELOCKTARGETTAG targettag;
		uint32		targettaghash;
		LWLock	   *partitionLock;

		tag = predlock->tag;
		target = tag.myTarget;
		targettag = target->tag;
		targettaghash = PredicateLockTargetTagHashCode(&targettag);
		partitionLock = PredicateLockHashPartitionLock(targettaghash);

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_delete(&predlock->targetLink);

		hash_search_with_hash_value(PredicateLockHash, &tag,
									PredicateLockHashCodeFromTargetHashCode(&tag,
																			targettaghash),
									HASH_REMOVE, NULL);
		if (summarize)
		{
			bool		found;

			/* 折叠进哑事务列表。 */
			tag.myXact = OldCommittedSxact;
			predlock = hash_search_with_hash_value(PredicateLockHash, &tag,
												   PredicateLockHashCodeFromTargetHashCode(&tag,
																						   targettaghash),
												   HASH_ENTER_NULL, &found);
			if (!predlock)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory"),
						 errhint("You might need to increase %s.", "max_pred_locks_per_transaction")));
			if (found)
			{
				Assert(predlock->commitSeqNo != 0);
				Assert(predlock->commitSeqNo != InvalidSerCommitSeqNo);
				if (predlock->commitSeqNo < sxact->commitSeqNo)
					predlock->commitSeqNo = sxact->commitSeqNo;
			}
			else
			{
				dlist_push_tail(&target->predicateLocks,
								&predlock->targetLink);
				dlist_push_tail(&OldCommittedSxact->predicateLocks,
								&predlock->xactLink);
				predlock->commitSeqNo = sxact->commitSeqNo;
			}
		}
		else
			RemoveTargetIfNoLongerUsed(target, targettaghash);

		LWLockRelease(partitionLock);
	}

	/*
	 * 与其逐条移除，不如在我们遍历完
	 * 该列表之后，重新初始化链表头。
	 */
	dlist_init(&sxact->predicateLocks);

	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);

	sxidtag.xid = sxact->topXid;
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* 释放全部 outConflicts（除非 'partial' 为 true） */
	if (!partial)
	{
		dlist_foreach_modify(iter, &sxact->outConflicts)
		{
			RWConflict	conflict =
				dlist_container(RWConflictData, outLink, iter.cur);

			if (summarize)
				conflict->sxactIn->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
			ReleaseRWConflict(conflict);
		}
	}

	/* 释放全部 inConflicts。 */
	dlist_foreach_modify(iter, &sxact->inConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		if (summarize)
			conflict->sxactOut->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
		ReleaseRWConflict(conflict);
	}

	/* 最后，去掉该 xid 以及事务本身的记录。 */
	if (!partial)
	{
		if (sxidtag.xid != InvalidTransactionId)
			hash_search(SerializableXidHash, &sxidtag, HASH_REMOVE, NULL);
		ReleasePredXact(sxact);
	}

	LWLockRelease(SerializableXactHashLock);
}

/*
 * 测试给定的顶层事务是否与
 * 我们的当前事务并发（重叠）。
 *
 * 无论如何，我们都需要为 SSI 识别顶层事务，因此把
 * 那个顶层事务传给本函数，以省去检查快照
 * subxip 数组的开销。
 */
static bool
XidIsConcurrent(TransactionId xid)
{
	Snapshot	snap;

	Assert(TransactionIdIsValid(xid));
	Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

	snap = GetTransactionSnapshot();

	if (TransactionIdPrecedes(xid, snap->xmin))
		return false;

	if (TransactionIdFollowsOrEquals(xid, snap->xmax))
		return true;

	return pg_lfind32(xid, snap->xip, snap->xcnt);
}

bool
CheckForSerializableConflictOutNeeded(Relation relation, Snapshot snapshot)
{
	if (!SerializationNeededForRead(relation, snapshot))
		return false;

	/* 检查是否已经有别人决定我们必须失败退出 */
	if (SxactIsDoomed(MySerializableXact))
	{
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict out checking."),
				 errhint("The transaction might succeed if retried.")));
	}

	return true;
}

/*
 * CheckForSerializableConflictOut
 *		表访问方法正在读取一个已被修改的元组。若它判定
 *		自己正在读的元组版本对我们不可见，就应当
 *		传入创建该版本的事务的顶层 xid。
 *		否则，若它判定该版本对我们可见，但已经被
 *		删除，或者因为更新而有了更新的版本，就
 *		应当传入执行修改的事务的顶层 xid。
 *
 * 本函数会检查与我们自己的事务是否重叠。若给定的
 * xid 同样是可串行化的，并且这些事务相互重叠（也就是它们看不见
 * 彼此的写），那么我们就有一条冲突出。
 */
void
CheckForSerializableConflictOut(Relation relation, TransactionId xid, Snapshot snapshot)
{
	SERIALIZABLEXIDTAG sxidtag;
	SERIALIZABLEXID *sxid;
	SERIALIZABLEXACT *sxact;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	/* 检查是否已经有别人决定我们必须失败退出 */
	if (SxactIsDoomed(MySerializableXact))
	{
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict out checking."),
				 errhint("The transaction might succeed if retried.")));
	}
	Assert(TransactionIdIsValid(xid));

	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;

	/*
	 * 为该顶层 xid 查找 sxact，或查找汇总信息。
	 */
	sxidtag.xid = xid;
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
	sxid = (SERIALIZABLEXID *)
		hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
	if (!sxid)
	{
		/*
		 * 在 "正常" 的 SSI 结构里没有找到该事务。检查它是否
		 * 已被推到 SLRU 存储中，作为 "旧的已提交" 事务。
		 */
		SerCommitSeqNo conflictCommitSeqNo;

		conflictCommitSeqNo = SerialGetMinConflictCommitSeqNo(xid);
		if (conflictCommitSeqNo != 0)
		{
			if (conflictCommitSeqNo != InvalidSerCommitSeqNo
				&& (!SxactIsReadOnly(MySerializableXact)
					|| conflictCommitSeqNo
					<= MySerializableXact->SeqNo.lastCommitBeforeSnapshot))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to read/write dependencies among transactions"),
						 errdetail_internal("Reason code: Canceled on conflict out to old pivot %u.", xid),
						 errhint("The transaction might succeed if retried.")));

			if (SxactHasSummaryConflictIn(MySerializableXact)
				|| !dlist_is_empty(&MySerializableXact->inConflicts))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to read/write dependencies among transactions"),
						 errdetail_internal("Reason code: Canceled on identification as a pivot, with conflict out to old committed transaction %u.", xid),
						 errhint("The transaction might succeed if retried.")));

			MySerializableXact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
		}

		/* 它不是可串行化的，或者因为别的原因并不重要。 */
		LWLockRelease(SerializableXactHashLock);
		return;
	}
	sxact = sxid->myXact;
	Assert(TransactionIdEquals(sxact->topXid, xid));
	if (sxact == MySerializableXact || SxactIsDoomed(sxact))
	{
		/* 不能与我们自己冲突，也不能与一个将会回滚的事务冲突。 */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	/*
	 * 我们有一条冲突出，指向另一个事务，而那个事务又有一条冲突出，指向一个
	 * 已汇总的事务。那个已汇总的事务必定
	 * 最先提交，而我们无法判断它的提交时刻相对于我们
	 * 取得快照的时刻，因此必须取消某个事务。
	 */
	if (SxactHasSummaryConflictOut(sxact))
	{
		if (!SxactIsPrepared(sxact))
		{
			sxact->flags |= SXACT_FLAG_DOOMED;
			LWLockRelease(SerializableXactHashLock);
			return;
		}
		else
		{
			LWLockRelease(SerializableXactHashLock);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on conflict out to old pivot."),
					 errhint("The transaction might succeed if retried.")));
		}
	}

	/*
	 * 若这是只读事务，并且执行写的事务已经
	 * 提交，而且它并没有一条读写依赖（rw-conflict）指向一个在它之前
	 * 就已经提交的事务，则没有冲突。
	 */
	if (SxactIsReadOnly(MySerializableXact)
		&& SxactIsCommitted(sxact)
		&& !SxactHasSummaryConflictOut(sxact)
		&& (!SxactHasConflictOut(sxact)
			|| MySerializableXact->SeqNo.lastCommitBeforeSnapshot < sxact->SeqNo.earliestOutConflictCommit))
	{
		/* 只读事务将表现为先运行。没有冲突。 */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	if (!XidIsConcurrent(xid))
	{
		/* 这次写已经处于我们的快照之中；没有冲突。 */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	if (RWConflictExists(MySerializableXact, sxact))
	{
		/* 我们不希望列表里出现重复的冲突记录。 */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	/*
	 * 标记这一冲突。但首先，若此冲突造成了危险
	 * 结构，就用 ereport 报告错误。
	 */
	FlagRWConflict(MySerializableXact, sxact);
	LWLockRelease(SerializableXactHashLock);
}

/*
 * 检查某个特定目标上进入方向的读写依赖冲突。它是下列例程的子程序：
 * CheckForSerializableConflictIn()。
 */
static void
CheckTargetForConflictsIn(PREDICATELOCKTARGETTAG *targettag)
{
	uint32		targettaghash;
	LWLock	   *partitionLock;
	PREDICATELOCKTARGET *target;
	PREDICATELOCK *mypredlock = NULL;
	PREDICATELOCKTAG mypredlocktag;
	dlist_mutable_iter iter;

	Assert(MySerializableXact != InvalidSerializableXact);

	/*
	 * 同一散列以及同一把 LW 锁同时适用于锁目标和锁本身。
	 */
	targettaghash = PredicateLockTargetTagHashCode(targettag);
	partitionLock = PredicateLockHashPartitionLock(targettaghash);
	LWLockAcquire(partitionLock, LW_SHARED);
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									targettag, targettaghash,
									HASH_FIND, NULL);
	if (!target)
	{
		/* 没有任何东西锁住这个目标；这里的工作到此结束。 */
		LWLockRelease(partitionLock);
		return;
	}

	/*
	 * 每个重叠事务所持的锁都代表一次冲突：一条
	 * 进入本事务的读写依赖。
	 */
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	dlist_foreach_modify(iter, &target->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, targetLink, iter.cur);
		SERIALIZABLEXACT *sxact = predlock->tag.myXact;

		if (sxact == MySerializableXact)
		{
			/*
			 * 若我们正在取得某个元组上的写锁，就不需要同一元组上的
			 * 谓词（SIREAD）锁。我们可以安全地移除
			 * 自己的 SIREAD 锁，但把这件事推迟到循环结束之后，
			 * 因为那需要把分区锁升级为排他模式。
			 *
			 * 不能在子事务内部使用这项优化，因为
			 * 子事务可能会回滚，那样我们就会在
			 * 顶层没有任何锁剩下。
			 */
			if (!IsSubTransaction()
				&& GET_PREDICATELOCKTARGETTAG_OFFSET(*targettag))
			{
				mypredlock = predlock;
				mypredlocktag = predlock->tag;
			}
		}
		else if (!SxactIsDoomed(sxact)
				 && (!SxactIsCommitted(sxact)
					 || TransactionIdPrecedes(GetTransactionSnapshot()->xmin,
											  sxact->finishedBefore))
				 && !RWConflictExists(sxact, MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

			/*
			 * 取得排他锁之后要重新检查，因为另一个
			 * 事务可能已经把冲突标记出来了。
			 */
			if (!SxactIsDoomed(sxact)
				&& (!SxactIsCommitted(sxact)
					|| TransactionIdPrecedes(GetTransactionSnapshot()->xmin,
											 sxact->finishedBefore))
				&& !RWConflictExists(sxact, MySerializableXact))
			{
				FlagRWConflict(sxact, MySerializableXact);
			}

			LWLockRelease(SerializableXactHashLock);
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
	}
	LWLockRelease(SerializableXactHashLock);
	LWLockRelease(partitionLock);

	/*
	 * 若找到了我们自己的、应当移除的 SIREAD 锁，现在就移除。
	 *
	 * 此时我们的事务已经对该
	 * 关系持有 RowExclusiveLock，因此若找到了元组上的谓词锁，丢掉它是可以的，
	 * 不必担心会再发生一次针对该元组的写，
	 * 发生在 MVCC 信息到达缓冲区之前。
	 */
	if (mypredlock != NULL)
	{
		uint32		predlockhashcode;
		PREDICATELOCK *rmpredlock;

		LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
		if (IsInParallelMode())
			LWLockAcquire(&MySerializableXact->perXactPredicateListLock, LW_EXCLUSIVE);
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);
		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		/*
		 * 若共享内存中的谓词锁尚未被移除，就把它移除，
		 * 在这些锁被放开的期间，它可能已经被删掉。发生这种情况的一条途径是
		 * autovacuum 正在清理某个索引。
		 */
		predlockhashcode = PredicateLockHashCodeFromTargetHashCode
			(&mypredlocktag, targettaghash);
		rmpredlock = (PREDICATELOCK *)
			hash_search_with_hash_value(PredicateLockHash,
										&mypredlocktag,
										predlockhashcode,
										HASH_FIND, NULL);
		if (rmpredlock != NULL)
		{
			Assert(rmpredlock == mypredlock);

			dlist_delete(&(mypredlock->targetLink));
			dlist_delete(&(mypredlock->xactLink));

			rmpredlock = (PREDICATELOCK *)
				hash_search_with_hash_value(PredicateLockHash,
											&mypredlocktag,
											predlockhashcode,
											HASH_REMOVE, NULL);
			Assert(rmpredlock == mypredlock);

			RemoveTargetIfNoLongerUsed(target, targettaghash);
		}

		LWLockRelease(SerializableXactHashLock);
		LWLockRelease(partitionLock);
		if (IsInParallelMode())
			LWLockRelease(&MySerializableXact->perXactPredicateListLock);
		LWLockRelease(SerializablePredicateListLock);

		if (rmpredlock != NULL)
		{
			/*
			 * 若本地锁表里有对应条目，就移除它。没有也没关系；
			 * 那表示这把锁已经被另一个后端转移到了新的
			 * 目标上。
			 */
			hash_search_with_hash_value(LocalPredicateLockHash,
										targettag, targettaghash,
										HASH_REMOVE, NULL);

			DecrementParentLocks(targettag);
		}
	}
}

/*
 * CheckForSerializableConflictIn
 *		我们正在写给定的元组。若这表明存在来自另一个可串行化事务的
 *		读写依赖（rw-conflict）入，就采取适当的行动。
 *
 * 对任何一个缺少参数的粒度，跳过检查。
 *
 * 若元组更新或删除与我们持有的谓词锁冲突，锁可以针对
 * 该元组所在的关系或页，也可以针对
 * 元组本身。
 */
void
CheckForSerializableConflictIn(Relation relation, ItemPointer tid, BlockNumber blkno)
{
	PREDICATELOCKTARGETTAG targettag;

	if (!SerializationNeededForWrite(relation))
		return;

	/* 检查是否已经有别人决定我们必须失败退出 */
	if (SxactIsDoomed(MySerializableXact))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict in checking."),
				 errhint("The transaction might succeed if retried.")));

	/*
	 * 我们正在做一次写，它现在或以后可能引起读写依赖（rw-conflict）。
	 * 把这一事实记下来。
	 */
	MyXactDidWrite = true;

	/*
	 * 重要的是，我们要从最细的粒度检查到
	 * 最粗的粒度，这样粒度提升才不会导致
	 * 我们漏掉一把锁。新的（更粗的）锁会在
	 * 旧的（更细的）锁被释放之前取得。
	 *
	 * 不可能在检查全部
	 * 粒度的整个过程中一直取得并持有同一把锁，因为每个目标都可能位于不同的分区。
	 */
	if (tid != NULL)
	{
		SET_PREDICATELOCKTARGETTAG_TUPLE(targettag,
										 relation->rd_locator.dbOid,
										 relation->rd_id,
										 ItemPointerGetBlockNumber(tid),
										 ItemPointerGetOffsetNumber(tid));
		CheckTargetForConflictsIn(&targettag);
	}

	if (blkno != InvalidBlockNumber)
	{
		SET_PREDICATELOCKTARGETTAG_PAGE(targettag,
										relation->rd_locator.dbOid,
										relation->rd_id,
										blkno);
		CheckTargetForConflictsIn(&targettag);
	}

	SET_PREDICATELOCKTARGETTAG_RELATION(targettag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	CheckTargetForConflictsIn(&targettag);
}

/*
 * CheckTableForSerializableConflictIn
 *		整张表正在经历一次 DDL 风格的逻辑批量删除，
 *		例如 TRUNCATE 或 DROP TABLE。若那引起了来自
 *		另一个可串行化事务的读写依赖（rw-conflict）入，就采取适当的行动。
 *
 * 虽然这些操作并不完全处在
 * 快照隔离的范围之内，它们仍然可以发生在可串行化事务内部，并且
 * 在逻辑上发生在任何曾看见被这些操作销毁的行的读之后，
 * 因此我们尽自己所能，在下列机制下把串行化做正确：
 * SSI。
 *
 * 传入的关系必须是堆关系。堆上任何粒度的任何谓词锁
 * 都会造成进入本事务的读写依赖（rw-conflict）。
 * 索引上的谓词锁并不要紧，因为它们存在的目的只是防范
 * 对索引的冲突插入，而这里是一次批量*删除*。
 * 当一张表被截断或删除时，其上的索引也会被截断
 * 或删除，到那时我们再处理索引上的锁。
 *
 * 删除或截断一张表，还需要丢掉堆元组或页上任何已经存在的谓词
 * 锁，因为它们马上就要消失。这件事
 * 应当在改动谓词锁之前完成，因为该事务
 * 可能因为冲突而回滚，那样的话这些锁变更
 * 就没有必要了。（目前我们实际上并不特意去丢掉
 * 已经被删除或截断的表上的现有锁。那样做可能
 * 带来一些假阳性，但似乎不值得为此费事。）
 */
void
CheckTableForSerializableConflictIn(Relation relation)
{
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCKTARGET *target;
	Oid			dbId;
	Oid			heapId;
	int			i;

	/*
	 * 若没有正在运行的可串行化事务，则迅速退出。
	 * 不加锁做此检查是安全的，因为调用者
	 * 持有该关系上的 ACCESS EXCLUSIVE 锁。在持有该锁期间，不能取得这里
	 * 会要紧的新锁。
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!SerializationNeededForWrite(relation))
		return;

	/*
	 * 我们正在做一次写，它现在或以后可能引起读写依赖（rw-conflict）。
	 * 把这一事实记下来。
	 */
	MyXactDidWrite = true;

	Assert(relation->rd_index == NULL); /* 不是索引关系 */

	dbId = relation->rd_locator.dbOid;
	heapId = relation->rd_id;

	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_SHARED);
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* 扫描目标列表 */
	hash_seq_init(&seqstat, PredicateLockTargetHash);

	while ((target = (PREDICATELOCKTARGET *) hash_seq_search(&seqstat)))
	{
		dlist_mutable_iter iter;

		/*
		 * 检查这是否是需要关注的目标。
		 */
		if (GET_PREDICATELOCKTARGETTAG_RELATION(target->tag) != heapId)
			continue;			/* 错误的关系 id */
		if (GET_PREDICATELOCKTARGETTAG_DB(target->tag) != dbId)
			continue;			/* 错误的数据库 id */

		/*
		 * 遍历此目标上的锁，并标记冲突。
		 */
		dlist_foreach_modify(iter, &target->predicateLocks)
		{
			PREDICATELOCK *predlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);

			if (predlock->tag.myXact != MySerializableXact
				&& !RWConflictExists(predlock->tag.myXact, MySerializableXact))
			{
				FlagRWConflict(predlock->tag.myXact, MySerializableXact);
			}
		}
	}

	/* 按相反顺序释放锁 */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));
	LWLockRelease(SerializablePredicateListLock);
}


/*
 * 在两个可串行化事务之间标记一条读写依赖。
 *
 * 调用者负责确保我们持有
 * 事务散列表上的一把 LW 锁。
 */
static void
FlagRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer)
{
	Assert(reader != writer);

	/* 首先看这个冲突会不会导致失败。 */
	OnConflict_CheckForSerializationFailure(reader, writer);

	/* 真正去把冲突标记上。 */
	if (reader == OldCommittedSxact)
		writer->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
	else if (writer == OldCommittedSxact)
		reader->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
	else
		SetRWConflict(reader, writer);
}

/*----------------------------------------------------------------------------
 * 我们即将向依赖图添加一条 RW 边——检查这样做不会
 * 引入危险结构；若会，就中止其中的一个
 * 事务。
 *
 * 只有当依赖图中存在危险结构时，才会发生序列化失败，
 * 危险结构位于依赖图之中：
 *
 *		Tin ------> Tpivot ------> Tout
 *			  rw			 rw
 *
 * 此外，Tout 必须最先提交。
 *
 * 还有一项优化：若 Tin 被声明为 READ ONLY（或者它提交时
 * 没有写任何东西），则只有当 Tout 在 Tin
 * 取得其快照之前就已经提交时，才可能出问题。
 *----------------------------------------------------------------------------
 */
static void
OnConflict_CheckForSerializationFailure(const SERIALIZABLEXACT *reader,
										SERIALIZABLEXACT *writer)
{
	bool		failure;

	Assert(LWLockHeldByMe(SerializableXactHashLock));

	failure = false;

	/*------------------------------------------------------------------------
	 * 检查已经提交的写者是否被标上了读写依赖（rw-conflict）出
	 * （W 上的冲突标志表示 T2 在 W 之前提交）：
	 *
	 *		R ------> W ------> T2
	 *			rw		  rw
	 *
	 * 那就是一个危险结构，所以我们必须中止。（既然写者
	 * 已经提交，我们自己就必定是读者。）
	 *------------------------------------------------------------------------
	 */
	if (SxactIsCommitted(writer)
		&& (SxactHasConflictOut(writer) || SxactHasSummaryConflictOut(writer)))
		failure = true;

	/*------------------------------------------------------------------------
	 * 检查写者是否已经变成枢纽，并且有一条指出的冲突，指向一个
	 * 已提交的事务（T2），而且 T2 最先提交：
	 *
	 *		R ------> W ------> T2
	 *			rw		  rw
	 *
	 * 因为 T2 必定最先提交，所以下列情形没有异常：
	 * - 读者在 T2 之前提交
	 * - 写者在 T2 之前提交
	 * - 读者是 READ ONLY 事务，并且读者与
	 *	 T2 并发（也就是读者在 T2 提交之前取得了自己的快照）
	 *
	 * 我们也在这里处理 T2 已经预备、但尚未提交的情形。
	 * 在那种情形下，T2 已经检查过冲突，因此若它
	 * 最先提交，从而使上面的冲突成为现实，它就已经来不及
	 * 再中止了。
	 *------------------------------------------------------------------------
	 */
	if (!failure && SxactHasSummaryConflictOut(writer))
		failure = true;
	else if (!failure)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &writer->outConflicts)
		{
			RWConflict	conflict =
				dlist_container(RWConflictData, outLink, iter.cur);
			SERIALIZABLEXACT *t2 = conflict->sxactIn;

			if (SxactIsPrepared(t2)
				&& (!SxactIsCommitted(reader)
					|| t2->prepareSeqNo <= reader->commitSeqNo)
				&& (!SxactIsCommitted(writer)
					|| t2->prepareSeqNo <= writer->commitSeqNo)
				&& (!SxactIsReadOnly(reader)
					|| t2->prepareSeqNo <= reader->SeqNo.lastCommitBeforeSnapshot))
			{
				failure = true;
				break;
			}
		}
	}

	/*------------------------------------------------------------------------
	 * 检查读者是否已经变成枢纽，并且对应的写者
	 * 已经提交（或已经预备）：
	 *
	 *		T0 ------> R ------> W
	 *			 rw		   rw
	 *
	 * 因为要出现异常，W 必定最先提交，所以没有
	 * 异常，如果：
	 * - T0 在写者之前提交
	 * - T0 是 READ ONLY，并且与写者重叠
	 *------------------------------------------------------------------------
	 */
	if (!failure && SxactIsPrepared(writer) && !SxactIsReadOnly(reader))
	{
		if (SxactHasSummaryConflictIn(reader))
		{
			failure = true;
		}
		else
		{
			dlist_iter	iter;

			/*
			 * 这里需要 unconstify，因为我们没有 const 版本的
			 * dlist_foreach()。
			 */
			dlist_foreach(iter, &unconstify(SERIALIZABLEXACT *, reader)->inConflicts)
			{
				const RWConflict conflict =
					dlist_container(RWConflictData, inLink, iter.cur);
				const SERIALIZABLEXACT *t0 = conflict->sxactOut;

				if (!SxactIsDoomed(t0)
					&& (!SxactIsCommitted(t0)
						|| t0->commitSeqNo >= writer->prepareSeqNo)
					&& (!SxactIsReadOnly(t0)
						|| t0->SeqNo.lastCommitBeforeSnapshot >= writer->prepareSeqNo))
				{
					failure = true;
					break;
				}
			}
		}
	}

	if (failure)
	{
		/*
		 * 我们必须杀掉一个事务，以免可能的异常
		 * 发生。若写者就是我们，可以直接 ereport()，从而引起
		 * 事务中止。否则就把写者标记为应当中止，
		 * 使它在试图提交时中止。不过，若写者
		 * 是一个已经预备的事务，预备已经完成，我们就再也无法中止它，
		 * 于是只好改为杀掉读者。
		 */
		if (MySerializableXact == writer)
		{
			LWLockRelease(SerializableXactHashLock);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on identification as a pivot, during write."),
					 errhint("The transaction might succeed if retried.")));
		}
		else if (SxactIsPrepared(writer))
		{
			LWLockRelease(SerializableXactHashLock);

			/* 若我们不是写者，那我们就一定是读者 */
			Assert(MySerializableXact == reader);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on conflict out to pivot %u, during read.", writer->topXid),
					 errhint("The transaction might succeed if retried.")));
		}
		writer->flags |= SXACT_FLAG_DOOMED;
	}
}

/*
 * PreCommit_CheckForSerializationFailure
 *		在一个可串行化事务中检查危险结构，
 *		检查发生在提交的时候。
 *
 * 每记录下一条冲突，我们就检查一次危险结构。
 * 提交时还可能出问题的唯一情况，是本事务处于枢纽的 "出"
 * 一侧，而 "入" 的一侧以及枢纽本身都还没有
 * 已提交。
 *
 * 若发现了危险结构，枢纽（近处的那个冲突）会被
 * 标记为应当失败，因为回滚另一个事务可能意味着
 * 我们失败了，却永远没有取得进展。本事务正在
 * 提交自己的写，因此让它提交能够保证有进展。若我们
 * 取消了远处的那个冲突，重试时它可能立刻再次失败。
 */
void
PreCommit_CheckForSerializationFailure(void)
{
	dlist_iter	near_iter;

	if (MySerializableXact == InvalidSerializableXact)
		return;

	Assert(IsolationIsSerializable());

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * 检查是否已经有别人决定我们必须失败退出。由于
	 * 我们在做部分释放时会设置自己的 DOOMED 标志，在该情形下应予以忽略。
	 * 情形。
	 */
	if (SxactIsDoomed(MySerializableXact) &&
		!SxactIsPartiallyReleased(MySerializableXact))
	{
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during commit attempt."),
				 errhint("The transaction might succeed if retried.")));
	}

	dlist_foreach(near_iter, &MySerializableXact->inConflicts)
	{
		RWConflict	nearConflict =
			dlist_container(RWConflictData, inLink, near_iter.cur);

		if (!SxactIsCommitted(nearConflict->sxactOut)
			&& !SxactIsDoomed(nearConflict->sxactOut))
		{
			dlist_iter	far_iter;

			dlist_foreach(far_iter, &nearConflict->sxactOut->inConflicts)
			{
				RWConflict	farConflict =
					dlist_container(RWConflictData, inLink, far_iter.cur);

				if (farConflict->sxactOut == MySerializableXact
					|| (!SxactIsCommitted(farConflict->sxactOut)
						&& !SxactIsReadOnly(farConflict->sxactOut)
						&& !SxactIsDoomed(farConflict->sxactOut)))
				{
					/*
					 * 通常我们会杀掉枢纽事务，以确保
					 * 失败的那个事务若被重试，我们仍能取得进展。
					 * 不过，若它已经预备，我们就不能杀掉它，因此
					 * 在那种情况下，我们改为让自己失败退出。
					 */
					if (SxactIsPrepared(nearConflict->sxactOut))
					{
						LWLockRelease(SerializableXactHashLock);
						ereport(ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								 errmsg("could not serialize access due to read/write dependencies among transactions"),
								 errdetail_internal("Reason code: Canceled on commit attempt with conflict in from prepared pivot."),
								 errhint("The transaction might succeed if retried.")));
					}
					nearConflict->sxactOut->flags |= SXACT_FLAG_DOOMED;
					break;
				}
			}
		}
	}

	MySerializableXact->prepareSeqNo = ++(PredXact->LastSxactCommitSeqNo);
	MySerializableXact->flags |= SXACT_FLAG_PREPARED;

	LWLockRelease(SerializableXactHashLock);
}

/*------------------------------------------------------------------------*/

/*
 * 两阶段提交支持
 */

/*
 * AtPrepare_Locks
 *		为 PREPARE 做准备：给当前持有的全部谓词锁
 *		制作两阶段提交状态文件中的记录。
 */
void
AtPrepare_PredicateLocks(void)
{
	SERIALIZABLEXACT *sxact;
	TwoPhasePredicateRecord record;
	TwoPhasePredicateXactRecord *xactRecord;
	TwoPhasePredicateLockRecord *lockRecord;
	dlist_iter	iter;

	sxact = MySerializableXact;
	xactRecord = &(record.data.xactRecord);
	lockRecord = &(record.data.lockRecord);

	if (MySerializableXact == InvalidSerializableXact)
		return;

	/* 为我们的 SERIALIZABLEXACT 生成一条事务记录 */
	record.type = TWOPHASEPREDICATERECORD_XACT;
	xactRecord->xmin = MySerializableXact->xmin;
	xactRecord->flags = MySerializableXact->flags;

	/*
	 * 注意，我们并不把出方向上的冲突列表放进
	 * 状态文件，因为即使在事务
	 * 完成预备之后，仍可能加入新的冲突。恢复时我们改为
	 * 采用一个保守的假设。
	 */

	RegisterTwoPhaseRecord(TWOPHASE_RM_PREDICATELOCK_ID, 0,
						   &record, sizeof(record));

	/*
	 * 为每一把锁生成一条锁记录。
	 *
	 * 为此，我们需要遍历自己 sxact 里的谓词锁列表，而不是
	 * 去用本地谓词锁表，因为后者
	 * 并不能保证是准确的。
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);

	/*
	 * 并行模式下不必取得 sxact->perXactPredicateListLock，
	 * 因为当我们正在
	 * 预备一个事务时，不可能还有并行工作进程在运行。
	 */
	Assert(!IsParallelWorker() && !ParallelContextActive());

	dlist_foreach(iter, &sxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);

		record.type = TWOPHASEPREDICATERECORD_LOCK;
		lockRecord->target = predlock->tag.myTarget->tag;

		RegisterTwoPhaseRecord(TWOPHASE_RM_PREDICATELOCK_ID, 0,
							   &record, sizeof(record));
	}

	LWLockRelease(SerializablePredicateListLock);
}

/*
 * PostPrepare_Locks
 *		成功 PREPARE 之后做清理。与非谓词的
 *		锁管理器不同，我们不需要把锁转移到一个哑的
 *		PGPROC，因为我们的 SERIALIZABLEXACT 无论如何都会继续
 *		留在那里。我们只需要清理自己的本地状态。
 */
void
PostPrepare_PredicateLocks(TransactionId xid)
{
	if (MySerializableXact == InvalidSerializableXact)
		return;

	Assert(SxactIsPrepared(MySerializableXact));

	MySerializableXact->pid = 0;
	MySerializableXact->pgprocno = INVALID_PGPROCNO;

	hash_destroy(LocalPredicateLockHash);
	LocalPredicateLockHash = NULL;

	MySerializableXact = InvalidSerializableXact;
	MyXactDidWrite = false;
}

/*
 * PredicateLockTwoPhaseFinish
 *		一旦某个预备事务
 *		提交或者中止，就释放它的谓词锁。
 */
void
PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit)
{
	SERIALIZABLEXID *sxid;
	SERIALIZABLEXIDTAG sxidtag;

	sxidtag.xid = xid;

	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	sxid = (SERIALIZABLEXID *)
		hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
	LWLockRelease(SerializableXactHashLock);

	/* 若它当初不是可串行化事务，就找不到这个 xid */
	if (sxid == NULL)
		return;

	/* 释放它所持的锁 */
	MySerializableXact = sxid->myXact;
	MyXactDidWrite = true;		/* 保守地假定我们执行过
								 * 某些写操作 */
	ReleasePredicateLocks(isCommit, false);
}

/*
 * 重新取得一把属于曾经被预备的事务的谓词锁。
 */
void
predicatelock_twophase_recover(TransactionId xid, uint16 info,
							   void *recdata, uint32 len)
{
	TwoPhasePredicateRecord *record;

	Assert(len == sizeof(TwoPhasePredicateRecord));

	record = (TwoPhasePredicateRecord *) recdata;

	Assert((record->type == TWOPHASEPREDICATERECORD_XACT) ||
		   (record->type == TWOPHASEPREDICATERECORD_LOCK));

	if (record->type == TWOPHASEPREDICATERECORD_XACT)
	{
		/* 按事务存放的记录。建立起 SERIALIZABLEXACT。 */
		TwoPhasePredicateXactRecord *xactRecord;
		SERIALIZABLEXACT *sxact;
		SERIALIZABLEXID *sxid;
		SERIALIZABLEXIDTAG sxidtag;
		bool		found;

		xactRecord = (TwoPhasePredicateXactRecord *) &record->data.xactRecord;

		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		sxact = CreatePredXact();
		if (!sxact)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory")));

		/* 预备事务的 vxid 是 InvalidBackendId/xid；没有 pid */
		sxact->vxid.backendId = InvalidBackendId;
		sxact->vxid.localTransactionId = (LocalTransactionId) xid;
		sxact->pid = 0;
		sxact->pgprocno = INVALID_PGPROCNO;

		/* 一个预备事务还没有提交 */
		sxact->prepareSeqNo = RecoverySerCommitSeqNo;
		sxact->commitSeqNo = InvalidSerCommitSeqNo;
		sxact->finishedBefore = InvalidTransactionId;

		sxact->SeqNo.lastCommitBeforeSnapshot = RecoverySerCommitSeqNo;

		/*
		 * 不必跟踪这一点；在被恢复的事务启动之时正在运行的事务，
		 * 现在都已经不再活动，可能的例外只是其他
		 * 预备事务，而我们并不关心那些事务是不是 RO_SAFE。
		 */
		dlist_init(&(sxact->possibleUnsafeConflicts));

		dlist_init(&(sxact->predicateLocks));
		dlist_node_init(&sxact->finishedLink);

		sxact->topXid = xid;
		sxact->xmin = xactRecord->xmin;
		sxact->flags = xactRecord->flags;
		Assert(SxactIsPrepared(sxact));
		if (!SxactIsReadOnly(sxact))
		{
			++(PredXact->WritableSxactCount);
			Assert(PredXact->WritableSxactCount <=
				   (MaxBackends + max_prepared_xacts));
		}

		/*
		 * 我们不知道该事务是否曾经有过任何冲突，因此
		 * 将保守地假定它既有冲突入，也有
		 * 冲突出，并用汇总冲突标志来表示这一点。
		 */
		dlist_init(&(sxact->outConflicts));
		dlist_init(&(sxact->inConflicts));
		sxact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
		sxact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;

		/* 登记该事务的 xid */
		sxidtag.xid = xid;
		sxid = (SERIALIZABLEXID *) hash_search(SerializableXidHash,
											   &sxidtag,
											   HASH_ENTER, &found);
		Assert(sxid != NULL);
		Assert(!found);
		sxid->myXact = (SERIALIZABLEXACT *) sxact;

		/*
		 * 更新全局 xmin。注意，与
		 * 登记一个普通事务相比，这是一种特殊情形，因为全局 xmin 可能会
		 * 向后移动。这是可以的，因为在恢复结束之前，我们不会
		 * 去完成任何事务，也不会创建任何非预备的
		 * 事务，因此不存在把信息丢掉的危险。
		 */
		if ((!TransactionIdIsValid(PredXact->SxactGlobalXmin)) ||
			(TransactionIdFollows(PredXact->SxactGlobalXmin, sxact->xmin)))
		{
			PredXact->SxactGlobalXmin = sxact->xmin;
			PredXact->SxactGlobalXminCount = 1;
			SerialSetActiveSerXmin(sxact->xmin);
		}
		else if (TransactionIdEquals(sxact->xmin, PredXact->SxactGlobalXmin))
		{
			Assert(PredXact->SxactGlobalXminCount > 0);
			PredXact->SxactGlobalXminCount++;
		}

		LWLockRelease(SerializableXactHashLock);
	}
	else if (record->type == TWOPHASEPREDICATERECORD_LOCK)
	{
		/* 锁记录。重新创建 PREDICATELOCK */
		TwoPhasePredicateLockRecord *lockRecord;
		SERIALIZABLEXID *sxid;
		SERIALIZABLEXACT *sxact;
		SERIALIZABLEXIDTAG sxidtag;
		uint32		targettaghash;

		lockRecord = (TwoPhasePredicateLockRecord *) &record->data.lockRecord;
		targettaghash = PredicateLockTargetTagHashCode(&lockRecord->target);

		LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		sxidtag.xid = xid;
		sxid = (SERIALIZABLEXID *)
			hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
		LWLockRelease(SerializableXactHashLock);

		Assert(sxid != NULL);
		sxact = sxid->myXact;
		Assert(sxact != InvalidSerializableXact);

		CreatePredicateLock(&lockRecord->target, targettaghash, sxact);
	}
}

/*
 * 准备与并行工作进程共享当前的 SERIALIZABLEXACT。
 * 返回一个句柄对象，供并行工作进程里的 AttachSerializableXact()
 * 使用。
 */
SerializableXactHandle
ShareSerializableXact(void)
{
	return MySerializableXact;
}

/*
 * 允许并行工作进程导入领导进程的 SERIALIZABLEXACT。
 */
void
AttachSerializableXact(SerializableXactHandle handle)
{

	Assert(MySerializableXact == InvalidSerializableXact);

	MySerializableXact = (SERIALIZABLEXACT *) handle;
	if (MySerializableXact != InvalidSerializableXact)
		CreateLocalPredicateLockHash();
}
