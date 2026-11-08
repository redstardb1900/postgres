/*-------------------------------------------------------------------------
 *
 * snapbuild.c
 *
 *	  基于 WAL 内容构建历史系统表快照的基础设施，用于    
 *	  解码 heapam.c 风格的值。                                     
 *	  WAL.
 *
 * NOTES:
 *
 * 我们构建的快照*只能*用于读取系统表内容，构建方式是 
 * 读取并解释 WAL 流。目标是构建行为与                     
 * XLogRecord 生成时新取得的 MVCC                                     
 * 快照相同的快照。                 
 *
 * 为构建这些快照，我们复用了为 Hot Standby 构建的
 * 基础设施。我们的内存快照与 HS 的不同，因为            
 * 需求不同。从 WAL 解码数据只需访问系统表和              
 *  (sys|rel|cat)cache，无需访问实际的用户表，因为            
 * 待解码的数据完全包含在 WAL                         
 * 记录中。此外，我们的快照与普通 MVCC 快照不同，       
 * 因为与它们不同，我们无法完全依赖 clog 和 pg_subtrans      
 * 来获取已提交事务的信息，因为在我们当前正在解码的   
 *  WAL 记录的视角看来，它们可能在将来提交。这一 
 * 定义的优点：我们只需阻止移除系统表行，普通表的行
 * 仍可被移除。这一点的实现方式是采用            
 * 复制槽机制。                                                
 *
 * 修改系统表的事务占比通常远小于只动用户数据的，     
 * 因此我们在 [xmin, xmax) 内跟踪已提交的系统表修改事务，
 * 而不是像普通快照那样跟踪所有正在运行的事务。注意   
 * 我们一般只关注已获取 xid 的事务。也就是说，对          
 * snapshot->(xmin, xmax) 之间，我们维护一份视为已提交      
 * 的事务列表，其余视为中止或进行中。这也使我们        
 * 无需在子事务提交之前关心它们，这意味着           
 * 与 HS 不同，本模块无需关心                                      
 * 子事务溢出（suboverflowed）                                        
 * 等类似问题。                                   
 *
 * 复杂之处在于：例如处理 DDL/DML 混合事务时，     
 * 需要能看见事务中系统表中间版本的快照。这靠     
 * CommandId/cmin/cmax 实现。但问题是为了节省空间，             
 * 的问题是只能存一个值（参见 combocid.c）。组合 CID   
 * 仅存于内存，故我们记录额外信息，   
 * 从而在可见性检查期间能取得原始的 (cmin, cmax) 对。  
 * 详情见上文 reorderbuffer.c 内                                    
 *  ResolveCminCmaxDuringDecoding                                          
 *  函数的注释。                         
 *
 * 为支持这一切，我们需要自己的可见性例程，因为常规
 * 例程为其他场景做了优化。      
 *
 * 要将常规系统表快照替换为解码快照，请使用      
 * SetupHistoricSnapshot() 与 TeardownHistoricSnapshot() 函数。 
 *
 *
 *
 * snapbuild 机制分多个阶段启动，下图描述了                 
 *  SnapBuild->state 的状态迁移：                               
 *
 *		   +-------------------------+
 *	  +----|		 START			 |-------------+
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |		   running_xacts #1					   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   v
 *	  |    |   BUILDING_SNAPSHOT	 |------------>|
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 *	  |					|						   |
 *	  | running_xacts #2, xacts from #1 finished   |
 *	  |					|						   |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   v
 *	  |    |	   FULL_SNAPSHOT	 |------------>|
 *	  |    +-------------------------+			   |
 *	  |					|						   |
 * running_xacts		|					   saved snapshot
 * with zero xacts		|				  at running_xacts's lsn
 *	  |					|						   |
 *	  | running_xacts with xacts from #2 finished  |
 *	  |					|						   |
 *	  |					v						   |
 *	  |    +-------------------------+			   |
 *	  +--->|SNAPBUILD_CONSISTENT	 |<------------+
 *		   +-------------------------+
 *
 * 机制初始处于 START 阶段。读到足够新（高于安全 xmin  
 *  水平线）的 xl_running_xacts 记录时，发生状态迁移。   
 * 如果在生成 xl_running_xacts 记录时没有正在运行的    
 * 事务，我们将直接进入 CONSISTENT 状态，否则切换到      
 *  BUILDING_SNAPSHOT 状态。拥有完整快照意味着此后开始的     
 * 所有事务都能被完整解码，但此前开始的则无法解码。 
 * 在 FULL_SNAPSHOT 中，一旦所有此前正在运行               
 * 的事务都已提交或中止，我们就会切换到 CONSISTENT   
 * 状态。                                      
 *
 * 只有在达到 CONSISTENT 状态之后提交的事务才会被重放，  
 * 即使它们可能在 FULL_SNAPSHOT 阶段就已开始。      
 * 这确保到达一个点：之前没有更改被导出，之后的    
 * 所有更改都会导出。该点是初始化复制的便利起点，    
 * 因此我们在该点导出快照，它*可以*用于读取普通    
 * 数据。                                                                
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/snapbuild.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/heapam_xlog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/snapbuild.h"
#include "storage/block.h"		/* 调试输出     */
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/standby.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"

/*
 * 此结构体保存快照构建机制的当前状态。         
 * 除了在头文件中有前向声明外，它并不对外暴露，因此 
 * 我们可以轻松修改其内容。                
 */
struct SnapBuild
{
	/* 首个完整快照构建进展到什么程度         */
	SnapBuildState state;

	/* 本模块用于分配内存的私有内存上下文。          */
	MemoryContext context;

	/* 小于此值的事务均已提交/中止            */
	TransactionId xmin;

	/* 大于等于此值的事务未提交          */
	TransactionId xmax;

	/*
	 * 不重放来自小于此 LSN 的提交。此项可从外部设置，   
	 * 但 snapbuild.c 内部也会推进它（绝不回退）。           
	 */
	XLogRecPtr	start_decoding_at;

	/*
	 * 启用两阶段解码时的 LSN，或创建复制槽时发现         
	 * 一致点时的 LSN。                        
	 *
	 * 此前因两阶段未启用或不在初始快照内而跳过   
	 * 的已预备事务需稍后连同 COMMIT PREPARED 一起发送，     
	 * 且它们必须位于此点之前。                               
	 *            
	 */
	XLogRecPtr	two_phase_at;

	/*
	 * 在 "xl_running_xacts" 信息表明不存在比此值更小的   
	 * 运行中 xid 之前，不要开始解码 WAL。                  
	 */
	TransactionId initial_xmin_horizon;

	/* 指示构建的是完整快照还是仅系统表快照。       */
	bool		building_full_snapshot;

	/*
	 * 可在当前时刻有效查看系统表状态的快照。          
	 */
	Snapshot	snapshot;

	/*
	 * 确知快照已序列化到的最后一个位置对应的 LSN。       
	 */
	XLogRecPtr	last_serialized_snapshot;

	/*
	 * 我们需要用可用快照等加以更新的 reorderbuffer。  
	 */
	ReorderBuffer *reorder;

	/*
	 * 初始快照构建下一阶段发生时的 TransactionId。            
	 * 未知（SNAPBUILD_START）或无需下一阶段时为              
	 * InvalidTransactionId。                             
	 */
	TransactionId next_phase_at;

	/*
	 * 可能有系统表改动并已提交的事务位于 xmin 与 xmax    
	 * 之间的数组。    
	 */
	struct
	{
		/* 已提交事务的数量         */
		size_t		xcnt;

		/* 已提交事务的可用空间             */
		size_t		xcnt_space;

		/*
		 * 达到 CONSISTENT 前，记录所有事务的提交，     
		 * 而非仅系统表改动事务。记录该转变发生的时点，
		 * 以便知道从此再也无法安全导出快照。           
		 */
		bool		includes_all_transactions;

		/*
		 * 已修改系统表的已提交事务所组成的数组。      
		 *
		 * 由于该数组频繁修改，我们*不*按 xidComparator 
		 * 顺序保存，而是在构建与分发                       
		 * 快照时才排序。   
		 *
		 * TODO: 这一理由是否很在理并不清楚。每次在达到
		 * 一致后再在此添加内容也需分发快照。按有     
		 * 序保存可能让清理更容易（但回卷处理更复 
		 * 杂）。如果在构建快照时的排序出现在性能剖析   
		 * 中，则应加以                                            
		 * 改进。                     
		 */
		TransactionId *xip;
	}			committed;

	/*
	 * 在快照序列化时已修改系统表且仍在运行的事务与  
	 * 子事务的数组。                             
	 *
	 * 我们通常依赖 HEAP2_NEW_CID 等 WAL 记录类型来判断         
	 * 事务是否改过系统表。但可能在恢复先前序列化的  
	 * 快照后，逻辑解码只见到该事务的提交记录，此时就 
	 * 会漏掉将 xid 加进快照，最终在查看系统表时用了  
	 * 错误的快照。                                                
	 * 为避免上述问题，我们   
	 *
	 * 会序列化已修改系统表且在快照序列化时仍             
	 * 在运行的事务。恢复快照时我们填充此数组，然后 
	 * 解码提交时参照它，确认相关事务是否修改过目        
	 * 录。当列表中的所有 xid 都老到不再有影响的程度    
	 * 时，我们便丢弃此数组。                                      
	 * 详见 SnapBuildPurgeOlderTxn。                         
	 */
	struct
	{
		/* 事务数量           */
		size_t		xcnt;

		/* 此数组必须按 xidComparator 顺序排序    */
		TransactionId *xip;
	}			catchange;
};

/*
 * 启动事务（这在导出快照时是必需的）会清除关于         
 * 先前所用 resowner 的记忆，所以我们在此保存它。         
 */
static ResourceOwner SavedResourceOwnerDuringExport = NULL;
static bool ExportInProgress = false;

/* ->committed 与 ->catchange 操作       */
static void SnapBuildPurgeOlderTxn(SnapBuild *builder);

/* 快照构建/操作/分发函数                      */
static Snapshot SnapBuildBuildSnapshot(SnapBuild *builder);

static void SnapBuildFreeSnapshot(Snapshot snap);

static void SnapBuildSnapIncRefcount(Snapshot snap);

static void SnapBuildDistributeSnapshotAndInval(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid);

static inline bool SnapBuildXidHasCatalogChanges(SnapBuild *builder, TransactionId xid,
												 uint32 xinfo);

/* SnapBuildProcessRunningXacts 的 xlog 读取辅助函数       */
static bool SnapBuildFindSnapshot(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running);
static void SnapBuildWaitSnapshot(xl_running_xacts *running, TransactionId cutoff);

/* 序列化函数         */
static void SnapBuildSerialize(SnapBuild *builder, XLogRecPtr lsn);
static bool SnapBuildRestore(SnapBuild *builder, XLogRecPtr lsn);
static void SnapBuildRestoreContents(int fd, char *dest, Size size, const char *path);

/*
 * 分配新快照构建器。     
 *
 * xmin_horizon 是这样的 xid：大于等于它即可确保未移除     
 * 任何系统表行；start_lsn 是要重放提交的 LSN。
 */
SnapBuild *
AllocateSnapshotBuilder(ReorderBuffer *reorder,
						TransactionId xmin_horizon,
						XLogRecPtr start_lsn,
						bool need_full_snapshot,
						XLogRecPtr two_phase_at)
{
	MemoryContext context;
	MemoryContext oldcontext;
	SnapBuild  *builder;

	/* 在自有上下文中分配内存，以便更好地核算     */
	context = AllocSetContextCreate(CurrentMemoryContext,
									"snapshot builder context",
									ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(context);

	builder = palloc0(sizeof(SnapBuild));

	builder->state = SNAPBUILD_START;
	builder->context = context;
	builder->reorder = reorder;
	/* 其余结构体成员由上面的 palloc0 清零初始化     */

	builder->committed.xcnt = 0;
	builder->committed.xcnt_space = 128;	/* 任意数值     */
	builder->committed.xip =
		palloc0(builder->committed.xcnt_space * sizeof(TransactionId));
	builder->committed.includes_all_transactions = true;

	builder->catchange.xcnt = 0;
	builder->catchange.xip = NULL;

	builder->initial_xmin_horizon = xmin_horizon;
	builder->start_decoding_at = start_lsn;
	builder->building_full_snapshot = need_full_snapshot;
	builder->two_phase_at = two_phase_at;

	MemoryContextSwitchTo(oldcontext);

	return builder;
}

/*
 * 释放快照构建器。
 */
void
FreeSnapshotBuilder(SnapBuild *builder)
{
	MemoryContext context = builder->context;

	/* 显式释放快照，其中包含一些错误检查         */
	if (builder->snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(builder->snapshot);
		builder->snapshot = NULL;
	}

	/* 其他资源经由内存上下文重置来释放         */
	MemoryContextDelete(context);
}

/*
 * 释放一个先前由我们构建且当前无引用的快照。    
 */
static void
SnapBuildFreeSnapshot(Snapshot snap)
{
	/* 确保传入的不是外部快照                  */
	Assert(snap->snapshot_type == SNAPSHOT_HISTORIC_MVCC);

	/* 确保没人修改过我们的快照   */
	Assert(snap->curcid == FirstCommandId);
	Assert(!snap->suboverflowed);
	Assert(!snap->takenDuringRecovery);
	Assert(snap->regd_count == 0);

	/* 可能性略高，因此即便无 c-assert 也会检查      */
	if (snap->copied)
		elog(ERROR, "cannot free a copied snapshot");

	if (snap->active_count)
		elog(ERROR, "cannot free an active snapshot");

	pfree(snap);
}

/*
 * 我们处于快照构建的哪个阶段？ 
 */
SnapBuildState
SnapBuildCurrentState(SnapBuild *builder)
{
	return builder->state;
}

/*
 * 返回首次启用两阶段解码时的 LSN。                   
 */
XLogRecPtr
SnapBuildGetTwoPhaseAt(SnapBuild *builder)
{
	return builder->two_phase_at;
}

/*
 * 设置启用两阶段解码时的 LSN。           
 */
void
SnapBuildSetTwoPhaseAt(SnapBuild *builder, XLogRecPtr ptr)
{
	builder->two_phase_at = ptr;
}

/*
 * 结束于 'ptr' 的事务内容是否应当解码？          
 */
bool
SnapBuildXactNeedsSkip(SnapBuild *builder, XLogRecPtr ptr)
{
	return ptr < builder->start_decoding_at;
}

/*
 * 增加快照的引用计数。  
 *
 * 在把快照交给某个外部资源时，或在把 Snapshot 设为       
 * builder->snapshot 时会用到。      
 */
static void
SnapBuildSnapIncRefcount(Snapshot snap)
{
	snap->active_count++;
}

/*
 * 减少快照的引用计数，引用计数降到零时释放快照。 
 *
 * 对外可见，这样获交了一个已 IncRef 的 Snapshot 的外部  
 * 资源可以方便地调整其引用计数。     
 */
void
SnapBuildSnapDecRefcount(Snapshot snap)
{
	/* 确保传入的不是外部快照                  */
	Assert(snap->snapshot_type == SNAPSHOT_HISTORIC_MVCC);

	/* 确保没人修改过我们的快照   */
	Assert(snap->curcid == FirstCommandId);
	Assert(!snap->suboverflowed);
	Assert(!snap->takenDuringRecovery);

	Assert(snap->regd_count == 0);

	Assert(snap->active_count > 0);

	/* 可能性略高，因此即便无 cassert 也会检查      */
	if (snap->copied)
		elog(ERROR, "cannot free a copied snapshot");

	snap->active_count--;
	if (snap->active_count == 0)
		SnapBuildFreeSnapshot(snap);
}

/*
 * 构建新快照，其基础是当前已提交的系统表修改     
 * 事务。    
 *
 * 进行中且访问过系统表的事务*不*允许修改这些快照； 
 * 它们必须复制快照并填入相应的 ->curcid 与                 
 * ->subxip/subxcnt 值。     
 */
static Snapshot
SnapBuildBuildSnapshot(SnapBuild *builder)
{
	Snapshot	snapshot;
	Size		ssize;

	Assert(builder->state >= SNAPBUILD_FULL_SNAPSHOT);

	ssize = sizeof(SnapshotData)
		+ sizeof(TransactionId) * builder->committed.xcnt
		+ sizeof(TransactionId) * 1 /* 顶层 xid   */ ;

	snapshot = MemoryContextAllocZero(builder->context, ssize);

	snapshot->snapshot_type = SNAPSHOT_HISTORIC_MVCC;

	/*
	 * 我们挪用了 SnapshotData 的 xip 与 subxip 字段的原本       
	 * 含义，使其更贴合需求。      
	 *
	 * 'xip' 数组中保存必须视为已提交的事务。由于我们 
	 * 只查看来自改过系统表的事务的元组，只需保存    
	 *  xmin 与 xmax 之间存在的少数几个事务（常常一个      
	 * 都没有）。                                              
	 *
	 * 被修改过系统表的事务用的快照还会使用 'subxip' 数组
	 * 来保存其顶层 xid 及全部子事务 xid，这样当某些行 
	 * 虽不在 xip 中却仍需可见时，我们能识别出来。       
	 * subxip 仅在快照被复制进系统表修改事务的上下文时  
	 * 被填充，因为否则我们会                               
	 * 在事务间共享快照。只要事务未修改过系统表，  
	 * 它就无需把任何未提交的行                                 
	 * 视为可见，因此并不需要                                    
	 * 这些 xid。       
	 *
	 * 两数组均按 qsort 排序，以便用 bsearch() 查找。  
	 */
	Assert(TransactionIdIsNormal(builder->xmin));
	Assert(TransactionIdIsNormal(builder->xmax));

	snapshot->xmin = builder->xmin;
	snapshot->xmax = builder->xmax;

	/* 保存此快照要视为已提交的全部事务                   */
	snapshot->xip =
		(TransactionId *) ((char *) snapshot + sizeof(SnapshotData));
	snapshot->xcnt = builder->committed.xcnt;
	memcpy(snapshot->xip,
		   builder->committed.xip,
		   builder->committed.xcnt * sizeof(TransactionId));

	/* 排序以便 bsearch()   */
	qsort(snapshot->xip, snapshot->xcnt, sizeof(TransactionId), xidComparator);

	/*
	 * 初始时 subxip 为空，即这是给不修改系统表      
	 * 的事务用的快照。必要时由                         
	 *  ReorderBufferCopySnap() 填充。   
	 */
	snapshot->subxcnt = 0;
	snapshot->subxip = NULL;

	snapshot->suboverflowed = false;
	snapshot->takenDuringRecovery = false;
	snapshot->copied = false;
	snapshot->curcid = FirstCommandId;
	snapshot->active_count = 0;
	snapshot->regd_count = 0;
	snapshot->snapXactCompletionCount = 0;

	return snapshot;
}

/*
 * 构建初始槽快照并将其转换为 HeapTupleSatisfiesMVCC 所      
 * 能理解的普通快照。             
 *
 * 该快照既可直接用于当前事务，也可导出后在别的     
 * 事务中载入使用。             
 */
Snapshot
SnapBuildInitialSnapshot(SnapBuild *builder)
{
	Snapshot	snap;
	TransactionId xid;
	TransactionId safeXid;
	TransactionId *newxip;
	int			newxcnt = 0;

	Assert(XactIsoLevel == XACT_REPEATABLE_READ);
	Assert(builder->building_full_snapshot);

	/* 不允许更旧的快照    */
	InvalidateCatalogSnapshot();	/* 即将覆盖 MyProc->xmin       */
	if (HaveRegisteredOrActiveSnapshot())
		elog(ERROR, "cannot build an initial slot snapshot when snapshots exist");
	Assert(!HistoricSnapshotActive());

	if (builder->state != SNAPBUILD_CONSISTENT)
		elog(ERROR, "cannot build an initial slot snapshot before reaching a consistent state");

	if (!builder->committed.includes_all_transactions)
		elog(ERROR, "cannot build an initial slot snapshot, not all transactions are monitored anymore");

	/* 以免覆盖已有的值                 */
	if (TransactionIdIsValid(MyProc->xmin))
		elog(ERROR, "cannot build an initial slot snapshot when MyProc->xmin already is valid");

	snap = SnapBuildBuildSnapshot(builder);

	/*
	 * 我们知道 snap->xmin 存活，这由逻辑 xmin 机制     
	 * 强制保证。因此能不加锁地这么做，因为只改   
	 * 我们自己的值。  
	 *
	 * 构建初始快照很昂贵，未强制执行的 xmin 水平线 
	 * 会带来糟糕的后果，因此务必反复确认水平线确实被  
	 * 强制执行了。      
	 */
	LWLockAcquire(ProcArrayLock, LW_SHARED);
	safeXid = GetOldestSafeDecodingTransactionId(false);
	LWLockRelease(ProcArrayLock);

	if (TransactionIdFollows(safeXid, snap->xmin))
		elog(ERROR, "cannot build an initial slot snapshot as oldest safe xid %u follows snapshot's xmin %u",
			 safeXid, snap->xmin);

	MyProc->xmin = snap->xmin;

	/* 在事务上下文中分配     */
	newxip = (TransactionId *)
		palloc(sizeof(TransactionId) * GetMaxSnapshotXidCount());

	/*
	 * snapbuild.c 以"倒置"方式构建事务，即它在 ->xip 中保存  
	 * 的是已提交事务而非进行中的事务。办法是把未提交
	 * 标记为进行中来构建经典快照。                     
	 * 开销可能不小。              
	 */
	for (xid = snap->xmin; NormalTransactionIdPrecedes(xid, snap->xmax);)
	{
		void	   *test;

		/*
		 * 按照解码快照的 ->xip 语义检查事务                 
		 * 是否已提交  
		 */
		test = bsearch(&xid, snap->xip, snap->xcnt,
					   sizeof(TransactionId), xidComparator);

		if (test == NULL)
		{
			if (newxcnt >= GetMaxSnapshotXidCount())
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("initial slot snapshot too large")));

			newxip[newxcnt++] = xid;
		}

		TransactionIdAdvance(xid);
	}

	/* 按需调整其余快照字段             */
	snap->snapshot_type = SNAPSHOT_MVCC;
	snap->xcnt = newxcnt;
	snap->xip = newxip;

	return snap;
}

/*
 * 导出一个快照，使其能在另一个会话中用 SET TRANSACTION    
 * SNAPSHOT.
 *
 * 为此我们需要在当前后端启动一个事务，因为导入方
 * 会检查源事务是否仍然开启，以确保 xmin 水平线此后     
 * 没有前进过。                               
 */
const char *
SnapBuildExportSnapshot(SnapBuild *builder)
{
	Snapshot	snap;
	char	   *snapname;

	if (IsTransactionOrTransactionBlock())
		elog(ERROR, "cannot export a snapshot from within a transaction");

	if (SavedResourceOwnerDuringExport)
		elog(ERROR, "can only export one snapshot at a time");

	SavedResourceOwnerDuringExport = CurrentResourceOwner;
	ExportInProgress = true;

	StartTransactionCommand();

	/* 似乎没有设置这些值的优雅 API      */
	XactIsoLevel = XACT_REPEATABLE_READ;
	XactReadOnly = true;

	snap = SnapBuildInitialSnapshot(builder);

	/*
	 * 构建出普通快照后，将其激活，并用常规机制     
	 * 来导出它。                   
	 */
	snapname = ExportSnapshot(snap);

	ereport(LOG,
			(errmsg_plural("exported logical decoding snapshot: \"%s\" with %u transaction ID",
						   "exported logical decoding snapshot: \"%s\" with %u transaction IDs",
						   snap->xcnt,
						   snapname, snap->xcnt)));
	return snapname;
}

/*
 * 确保存在快照，若无则为当前事务构建一个。            
 */
Snapshot
SnapBuildGetOrBuildSnapshot(SnapBuild *builder)
{
	Assert(builder->state == SNAPBUILD_CONSISTENT);

	/* 仅在没有预构建快照时才构建新快照          */
	if (builder->snapshot == NULL)
	{
		builder->snapshot = SnapBuildBuildSnapshot(builder);
		/* 为快照构建器增加引用计数       */
		SnapBuildSnapIncRefcount(builder->snapshot);
	}

	return builder->snapshot;
}

/*
 * 如果存在先前由 SnapBuildExportSnapshot() 导出的快照，  
 * 则将其重置。中止先前启动的事务，并把资源所有者 
 * 恢复为原值。               
 */
void
SnapBuildClearExportedSnapshot(void)
{
	ResourceOwner tmpResOwner;

	/* 无导出内容，这是常见情形     */
	if (!ExportInProgress)
		return;

	if (!IsTransactionState())
		elog(ERROR, "clearing exported snapshot in wrong transaction state");

	/*
	 * AbortCurrentTransaction() 负责重置快照状态，所以请记住 
	 *  SavedResourceOwnerDuringExport。         
	 */
	tmpResOwner = SavedResourceOwnerDuringExport;

	/* 确保从未发生过任何事情          */
	AbortCurrentTransaction();

	CurrentResourceOwner = tmpResOwner;
}

/*
 * 在事务中止期间清除快照导出状态。     
 */
void
SnapBuildResetExportedSnapshotState(void)
{
	SavedResourceOwnerDuringExport = NULL;
	ExportInProgress = false;
}

/*
 * 按快照构建器的当前状态处理单个堆更改的效果，          
 * 并返回在 (xid, lsn) 处所做的更改能否被                       
 * 解码。  
 */
bool
SnapBuildProcessChange(SnapBuild *builder, TransactionId xid, XLogRecPtr lsn)
{
	/*
	 * 尚未构建快照时我们无法处理事务中的数据，       
	 * 故不保存它们。    
	 */
	if (builder->state < SNAPBUILD_FULL_SNAPSHOT)
		return false;

	/*
	 * 跟踪信息不足而无法解码的事务中的更改毫无意义。  
	 * 这意味着这些事务在我们进入 SNAPBUILD_FULL_SNAPSHOT        
	 *  状态之前就已经开始了。            
	 */
	if (builder->state < SNAPBUILD_CONSISTENT &&
		TransactionIdPrecedes(xid, builder->next_phase_at))
		return false;

	/*
	 * 如果 reorderbuffer 还没有快照，现在就添加一个，解码 
	 * 当前处理的更改时会需要它。                   
	 */
	if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, xid))
	{
		/* 仅在没有预构建快照时才构建新快照          */
		if (builder->snapshot == NULL)
		{
			builder->snapshot = SnapBuildBuildSnapshot(builder);
			/* 为快照构建器增加引用计数       */
			SnapBuildSnapIncRefcount(builder->snapshot);
		}

		/*
		 * 为接收此快照的事务增加引用计数。                
		 * 。    
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);
		ReorderBufferSetBaseSnapshot(builder->reorder, xid, lsn,
									 builder->snapshot);
	}

	return true;
}

/*
 * 读取 xl_heap_new_cid 记录后处理 CommandId/组合 CID。          
 * 这意味着某个事务曾以某种形式写入系统表。         
 * 。      
 */
void
SnapBuildProcessNewCid(SnapBuild *builder, TransactionId xid,
					   XLogRecPtr lsn, xl_heap_new_cid *xlrec)
{
	CommandId	cid;

	/*
	 * 仅系统表元组被修改时记录 new_cid，故把事务标记 
	 * 为包含系统表改动。                    
	 */
	ReorderBufferXidSetCatalogChanges(builder->reorder, xid, lsn);

	ReorderBufferAddNewTupleCids(builder->reorder, xlrec->top_xid, lsn,
								 xlrec->target_locator, xlrec->target_tid,
								 xlrec->cmin, xlrec->cmax,
								 xlrec->combocid);

	/* 推算新的命令 id     */
	if (xlrec->cmin != InvalidCommandId &&
		xlrec->cmax != InvalidCommandId)
		cid = Max(xlrec->cmin, xlrec->cmax);
	else if (xlrec->cmax != InvalidCommandId)
		cid = xlrec->cmax;
	else if (xlrec->cmin != InvalidCommandId)
		cid = xlrec->cmin;
	else
	{
		cid = InvalidCommandId; /* 安抚编译器  */
		elog(ERROR, "xl_heap_new_cid record without a valid CommandId");
	}

	ReorderBufferAddNewCommandId(builder->reorder, xid, lsn, cid + 1);
}

/*
 * 为解码中且进行中的事务添加 Snapshot 与失效消息，   
 * 使它们能看到刚提交的事务所做的新系统表内容。          
 * 这样做是必需的，因为这些进行中的事务从现在起将使用 
 * 新系统表内容（至少一切操作都需与更新后的系统表    
 * 内容兼容）。                                                     
 * 。               
 */
static void
SnapBuildDistributeSnapshotAndInval(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid)
{
	dlist_iter	txn_i;
	ReorderBufferTXN *txn;

	/*
	 * 遍历所有顶层事务。其中可能包含              
	 * 尚不知其为子事务的事务，没关系，它们只会多拿 
	 * 一个不必要的快照，失效消息入队而已。            
	 * 。    
	 */
	dlist_foreach(txn_i, &builder->reorder->toplevel_by_lsn)
	{
		txn = dlist_container(ReorderBufferTXN, node, txn_i.cur);

		Assert(TransactionIdIsValid(txn->xid));

		/*
		 * 若还没有基础快照，说明此事务中没有任何更改，
		 * 因而也尚不需要任何快照。首个更改入队时我们  
		 * 会添加快照。                                          
		 *
		 * 同样，基础快照未设置的事务无需添加失效消息。  
		 * 基础快照一旦建好，其中包含改过系统表的已提交  
		 * 事务的 xid，从而反映新的系统表内容。现有的    
		 * 系统表缓存在处理完改动系统表事务的失效消息
		 * 后，其实就已失效。                                      
		 * 这样能确保在解码期间构建出                     
		 * 全新的缓存。                                         
		 *
		 * 注意：这对子事务同样正确，因为            
		 * ReorderBufferAssignChild() 会负责把基础快照转移给顶层 
		 * 事务；遍历更改队列时，我们会从子事务           
		 * 处取得更改。                   
		 */
		if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, txn->xid))
			continue;

		/*
		 * 无需为已预备事务添加快照或失效消息，    
		 * 因为它们不应看到新的系统表内容。             
		 */
		if (rbtxn_prepared(txn) || rbtxn_skip_prepared(txn))
			continue;

		elog(DEBUG2, "adding a new snapshot and invalidations to %u at %X/%X",
			 txn->xid, LSN_FORMAT_ARGS(lsn));

		/*
		 * 为接收此快照的事务增加其引用计数。                
		 * 。      
		 */
		SnapBuildSnapIncRefcount(builder->snapshot);
		ReorderBufferAddSnapshot(builder->reorder, txn->xid, lsn,
								 builder->snapshot);

		/*
		 * 向进行中事务的 reorderbuffer 添加失效消息，     
		 * 当前已提交的事务除外，其失效消息                   
		 * 会在最后执行。                 
		 *
		 * 这是必需的，否则最终会用到陈旧的 catcache 内容  
		 * ——由当前事务构建的内容，即便其解码已完成。
		 * 本应因并发的系统表改动事务                     
		 * 而失效。         
		 *
		 * 只分发由当前已提交事务所产生的那批失效消息。
		 * 从其他事务收到的失效消息已传播给相关的进行 
		 * 中事务。该事务也已处理过这些失效，            
		 * 从而确保                                                   
		 * 后续事务能观察到一致的                                 
		 * 缓存状态。          
		 */
		if (txn->xid != xid)
		{
			uint32 ninvalidations;
			SharedInvalidationMessage *msgs = NULL;

			ninvalidations = ReorderBufferGetInvalidations(builder->reorder,
														   xid, &msgs);

			if (ninvalidations > 0)
			{
				Assert(msgs != NULL);

				ReorderBufferAddDistributedInvalidations(builder->reorder,
														 txn->xid, lsn,
														 ninvalidations, msgs);
			}
		}
	}
}

/*
 * 记录一个新近提交的系统表改动事务。                 
 */
static void
SnapBuildAddCommittedTxn(SnapBuild *builder, TransactionId xid)
{
	Assert(TransactionIdIsValid(xid));

	if (builder->committed.xcnt == builder->committed.xcnt_space)
	{
		builder->committed.xcnt_space = builder->committed.xcnt_space * 2 + 1;

		elog(DEBUG1, "increasing space for committed transactions to %u",
			 (uint32) builder->committed.xcnt_space);

		builder->committed.xip = repalloc(builder->committed.xip,
										  builder->committed.xcnt_space * sizeof(TransactionId));
	}

	/*
	 * TODO: 与其每次构建快照时才排序，不如让数组      
	 * 在此处保持有序。另一方面，带子事务的事务提交 
	 * 时这里会被反复调用。                                         
	 */
	builder->committed.xip[builder->committed.xcnt++] = xid;
}

/*
 * 清除关于那些被视为已提交或包含系统表改动、且小于 ->xmin
 * 的事务的信息。这些信息再也不会经 ->committed 或      
 * ->catchange 数组被检查。已提交的 xid 则由                      
 *  clog 机制检查。              
 *
 * 理想情况下，事务一结束（提交/中止）就应把它从  
 *  catchange 数组中移除，但这可能代价高昂，因为需           
 * 维护 xid 顺序。        
 */
static void
SnapBuildPurgeOlderTxn(SnapBuild *builder)
{
	int			off;
	TransactionId *workspace;
	int			surviving_xids = 0;

	/* 尚未就绪  */
	if (!TransactionIdIsNormal(builder->xmin))
		return;

	/* TODO: 有无比复制后遍历更优雅的算法？     */
	workspace =
		MemoryContextAlloc(builder->context,
						   builder->committed.xcnt * sizeof(TransactionId));

	/* 把仍有关联的 xid 复制到工作区         */
	for (off = 0; off < builder->committed.xcnt; off++)
	{
		if (NormalTransactionIdPrecedes(builder->committed.xip[off],
										builder->xmin))
			;					/* 移除 */
		else
			workspace[surviving_xids++] = builder->committed.xip[off];
	}

	/* 把工作区复制回持久状态       */
	memcpy(builder->committed.xip, workspace,
		   surviving_xids * sizeof(TransactionId));

	elog(DEBUG3, "purged committed transactions from %u to %u, xmin: %u, xmax: %u",
		 (uint32) builder->committed.xcnt, (uint32) surviving_xids,
		 builder->xmin, builder->xmax);
	builder->committed.xcnt = surviving_xids;

	pfree(workspace);

	/*
	 * 也要清除 ->catchange 中的 xid。清除后的数组也必须按   
	 *  xidComparator 序。  
	 */
	if (builder->catchange.xcnt > 0)
	{
		/*
		 * 由于 catchange.xip 已有序，我们可以找出仍有关联的 
		 *  xid 的下界。     
		 */
		for (off = 0; off < builder->catchange.xcnt; off++)
		{
			if (TransactionIdFollowsOrEquals(builder->catchange.xip[off],
											 builder->xmin))
				break;
		}

		surviving_xids = builder->catchange.xcnt - off;

		if (surviving_xids > 0)
		{
			memmove(builder->catchange.xip, &(builder->catchange.xip[off]),
					surviving_xids * sizeof(TransactionId));
		}
		else
		{
			pfree(builder->catchange.xip);
			builder->catchange.xip = NULL;
		}

		elog(DEBUG3, "purged catalog modifying transactions from %u to %u, xmin: %u, xmax: %u",
			 (uint32) builder->catchange.xcnt, (uint32) surviving_xids,
			 builder->xmin, builder->xmax);
		builder->catchange.xcnt = surviving_xids;
	}
}

/*
 * 处理事务提交时需要完成的一切事宜                  
 */
void
SnapBuildCommitTxn(SnapBuild *builder, XLogRecPtr lsn, TransactionId xid,
				   int nsubxacts, TransactionId *subxacts, uint32 xinfo)
{
	int			nxact;

	bool		needs_snapshot = false;
	bool		needs_timetravel = false;
	bool		sub_needs_timetravel = false;

	TransactionId xmax = xid;

	/*
	 * 先于 BUILDING_SNAPSHOT 的事务既不会被解码，也不会     
	 * 进入任何快照。因此我们无需记录任何内容。          
	 */
	if (builder->state == SNAPBUILD_START ||
		(builder->state == SNAPBUILD_BUILDING_SNAPSHOT &&
		 TransactionIdPrecedes(xid, builder->next_phase_at)))
	{
		/* 确保只重放此点之后的提交                     */
		if (builder->start_decoding_at <= lsn)
			builder->start_decoding_at = lsn + 1;
		return;
	}

	if (builder->state < SNAPBUILD_CONSISTENT)
	{
		/* 确保只重放此点之后的提交                     */
		if (builder->start_decoding_at <= lsn)
			builder->start_decoding_at = lsn + 1;

		/*
		 * 若要构建可导出的快照，则强制跟踪 xid，即使该 
		 * 事务本身并未修改过系统表。      
		 */
		if (builder->building_full_snapshot)
		{
			needs_timetravel = true;
		}
	}

	for (nxact = 0; nxact < nsubxacts; nxact++)
	{
		TransactionId subxid = subxacts[nxact];

		/*
		 * 子事务若修改系统表，则加入基础快照；我们不   
		 * 区分它与顶层事务。                
		 */
		if (SnapBuildXidHasCatalogChanges(builder, subxid, xinfo))
		{
			sub_needs_timetravel = true;
			needs_snapshot = true;

			elog(DEBUG1, "found subtransaction %u:%u with catalog changes",
				 xid, subxid);

			SnapBuildAddCommittedTxn(builder, subxid);

			if (NormalTransactionIdFollows(subxid, xmax))
				xmax = subxid;
		}

		/*
		 * 若强制时间旅行，还需子事务可见性信息，所以
		 * 要跟踪子事务的状态，即使其并未修改系统表。    
		 * 这种情况下无需分发                                      
		 * 快照。 
		 */
		else if (needs_timetravel)
		{
			SnapBuildAddCommittedTxn(builder, subxid);
			if (NormalTransactionIdFollows(subxid, xmax))
				xmax = subxid;
		}
	}

	/* 顶层事务修改了系统表时需要快照        */
	if (SnapBuildXidHasCatalogChanges(builder, xid, xinfo))
	{
		elog(DEBUG2, "found top level transaction %u, with catalog changes",
			 xid);
		needs_snapshot = true;
		needs_timetravel = true;
		SnapBuildAddCommittedTxn(builder, xid);
	}
	else if (sub_needs_timetravel)
	{
		/* 顶层事务也要跟踪，仅子事务本身没有意义  */
		elog(DEBUG2, "forced transaction %u to do timetravel due to one of its subtransactions",
			 xid);
		needs_timetravel = true;
		SnapBuildAddCommittedTxn(builder, xid);
	}
	else if (needs_timetravel)
	{
		elog(DEBUG2, "forced transaction %u to do timetravel", xid);

		SnapBuildAddCommittedTxn(builder, xid);
	}

	if (!needs_timetravel)
	{
		/* 记录下从此无法再导出通用快照              */
		builder->committed.includes_all_transactions = false;
	}

	Assert(!needs_snapshot || needs_timetravel);

	/*
	 * 调整快照构建器的 xmax，只针对已提交且修改了     
	 * 系统表的事务这样做；其余事务与我们无关，因为    
	 * 我们永远不会查看相应的行。          
	 */
	if (needs_timetravel &&
		(!TransactionIdIsValid(builder->xmax) ||
		 TransactionIdFollowsOrEquals(xmax, builder->xmax)))
	{
		builder->xmax = xmax;
		TransactionIdAdvance(builder->xmax);
	}

	/* 若还有任何理由构建历史快照，就现在进行     */
	if (needs_snapshot)
	{
		/*
		 * 若尚未构建出完整快照，就没有必要将其分发出    
		 * 去，反正它不会（也无法）被使用。  
		 */
		if (builder->state < SNAPBUILD_FULL_SNAPSHOT)
			return;

		/*
		 * 递减快照构建器对旧快照的引用计数，注意若它   
		 * 此前已分发给 reorderbuffer，则该快照仍会被   
		 * 使用。             
		 */
		if (builder->snapshot)
			SnapBuildSnapDecRefcount(builder->snapshot);

		builder->snapshot = SnapBuildBuildSnapshot(builder);

		/* 可能需要执行失效处理，加入快照        */
		if (!ReorderBufferXidHasBaseSnapshot(builder->reorder, xid))
		{
			SnapBuildSnapIncRefcount(builder->snapshot);
			ReorderBufferSetBaseSnapshot(builder->reorder, xid, lsn,
										 builder->snapshot);
		}

		/* 快照构建器针对新快照的引用计数         */
		SnapBuildSnapIncRefcount(builder->snapshot);

		/*
		 * 向当前运行中的事务加入新的系统表快照      
		 * 与失效消息。             
		 */
		SnapBuildDistributeSnapshotAndInval(builder, lsn, xid);
	}
}

/*
 * 检查 reorder buffer 与快照，判断给定事务是否修改             
 * 了系统表。   
 */
static inline bool
SnapBuildXidHasCatalogChanges(SnapBuild *builder, TransactionId xid,
							  uint32 xinfo)
{
	if (ReorderBufferXidHasCatalogChanges(builder->reorder, xid))
		return true;

	/*
	 * 修改过系统表的事务必须有失效信息                  
	 * 。  
	 */
	if (!(xinfo & XACT_XINFO_HAS_INVALS))
		return false;

	/* 检查 catchange XID 数组   */
	return ((builder->catchange.xcnt > 0) &&
			(bsearch(&xid, builder->catchange.xip, builder->catchange.xcnt,
					 sizeof(TransactionId), xidComparator) != NULL));
}

/* -----------------------------------
 * 处理 xlog 记录的快照构建函数              
 * -----------------------------------
 */

/*
 * 处理 running xacts 记录，先用其信息构建历史               
 * 快照，之后释放不再需要的资                            
 * 源。  
 */
void
SnapBuildProcessRunningXacts(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running)
{
	ReorderBufferTXN *txn;
	TransactionId xmin;

	/*
	 * 若尚未达到一致，则检查记录能否让我们更接近  
	 * 一致。若已达到一致，则把我们的快照转储出来，  
	 * 以便其他人或我们在重启后能够使用它。    
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
	{
		/* 若此刻执行清理尚无意义，则返回 false              */
		if (!SnapBuildFindSnapshot(builder, lsn, running))
			return;
	}
	else
		SnapBuildSerialize(builder, lsn);

	/*
	 * 根据 running xacts 信息更新所关注的 xid 范围。
	 * 我们不通过它来增大 ->xmax，因为一旦进入一致         
	 * 状态，我们自己就能做到且高效得多，因为只需      
	 * 对系统表事务这样做，因为我们只查看那些          
	 * 事务。               
	 *
	 * 注意：只有修改系统表的事务提交时才会增大 xmax     
	 * （参见 SnapBuildCommitTxn）。正因如此，xmax 可能      
	 * 小于 xmin，这看起来奇怪，但既正确又实际更高效，   
	 * 命中 heapam_visibility.c 快速路径  
	 */
	builder->xmin = running->oldestRunningXid;

	/* 移除我们不再需要跟踪的事务                     */
	SnapBuildPurgeOlderTxn(builder);

	/*
	 * 为当前复制槽推进 xmin 下限，让清理（vacuum）     
	 * 能够回收该槽一直保护的元组。                  
	 *
	 * reorderbuffer 当前运行快照中可能带有 xmin；         
	 * 若有就使用它。若无，则只需考虑我们稍后产生      
	 * 的快照，它们不可能小于当前所读记录中最老的        
	 * 运行 xid。                
	 */
	xmin = ReorderBufferGetOldestXmin(builder->reorder);
	if (xmin == InvalidTransactionId)
		xmin = running->oldestRunningXid;
	elog(DEBUG3, "xmin: %u, xmax: %u, oldest running: %u, oldest xmin: %u",
		 builder->xmin, builder->xmax, running->oldestRunningXid, xmin);
	LogicalIncreaseXminForSlot(lsn, xmin);

	/*
	 * 还要告知复制槽可以从何处重新开始解码。我们        
	 * 不想每次提交都这样做，因为改动意味着对逻辑槽  
	 * 的状态文件做 fsync，因此只在每次看到 running xacts  
	 * 记录时才做。   
	 *
	 * 做法是找出最老的在途事务（以其相关记录的第一    
	 * 条 LSN 确定）。每个事务都会记住其开始之前我  
	 * 们最后一次把快照存盘的位置。该点就是我们能够   
	 * 重新开始的地方。                           
	 */

	/*
	 * 若尚未一致，则无法得知序列化快照的           
	 * 位置。  
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
		return;

	txn = ReorderBufferGetOldestTXN(builder->reorder);

	/*
	 * 最老的在途事务可能始于我们尚未序列化任何内   
	 * 容之时，因为当时尚未达到一致状态。       
	 */
	if (txn != NULL && txn->restart_decoding_lsn != InvalidXLogRecPtr)
		LogicalIncreaseRestartDecodingForSlot(lsn, txn->restart_decoding_lsn);

	/*
	 * 没有在途事务；若有，可复用最后一次序列化的快   
	 * 照。      
	 */
	else if (txn == NULL &&
			 builder->reorder->current_restart_decoding_lsn != InvalidXLogRecPtr &&
			 builder->last_serialized_snapshot != InvalidXLogRecPtr)
		LogicalIncreaseRestartDecodingForSlot(lsn,
											  builder->last_serialized_snapshot);
}


/*
 * 构建一个能够解码系统表的快照的开端。               
 *
 * SnapBuildProcessRunningXacts() 的辅助函数，适用于尚未      
 * 一致时  
 *
 * 若使用 xl_running_xacts 记录做内部维护/清理还有              
 * 意义，则返回 true。        
 */
static bool
SnapBuildFindSnapshot(SnapBuild *builder, XLogRecPtr lsn, xl_running_xacts *running)
{
	LogicalDecodingContext *ctx = (LogicalDecodingContext *) builder->reorder->private_data;

	/* ---
	 * 利用当前正在运行事务的信息，增量地构建系统表   
	 * 解码快照。有几种可行的做法：                            
	 *
	 * a) 插入 xl_running_xacts 记录时没有正在运行的             
	 *	  事务，此时立即跳到 CONSISTENT。在等待 c) 的子      
	 *	  状态时可能遇到这种情形。   
	 *
	 * b) 本次（前次运行）或其他解码槽曾把可用的快  
	 *	  照序列化到了磁盘，我们可以使用它。在寻找变更   
	 *	  解码起点时无法使用这种方法，因为 restart LSN 会   
	 *	  成为任意 LSN，而我们需要先找到起点才能提取       
	 *	  不会看到部分事务数据的变更。另外，当槽需要      
	 *	  用于导出或直接使用的完整快照时，也无法使用此    
	 *	  方法，因为该快照只包含修改系统表的事务。           
	 *
	 * c) 先为系统表元组增量构建快照                
	 *	  （BUILDING_SNAPSHOT），这需要所有已在途的事务 
	 *	  完成。此后开始的每个事务（FULL_SNAPSHOT 状态）
	 *	  都有足够信息可被解码。但对更老的运行事务     
	 *	  而言尚无可用快照，因此须等它们全部完成才    
	 *	  会达到 CONSISTENT。                                         
	 * ---
	 */

	/*
	 * xl_running_xacts 记录比我们能用的更老，可能已        
	 * 没有全部所需的系统表行。    
	 */
	if (TransactionIdIsNormal(builder->initial_xmin_horizon) &&
		NormalTransactionIdPrecedes(running->oldestRunningXid,
									builder->initial_xmin_horizon))
	{
		ereport(DEBUG1,
				(errmsg_internal("skipping snapshot at %X/%X while building logical decoding snapshot, xmin horizon too low",
								 LSN_FORMAT_ARGS(lsn)),
				 errdetail_internal("initial xmin horizon of %u vs the snapshot's %u",
									builder->initial_xmin_horizon, running->oldestRunningXid)));


		SnapBuildWaitSnapshot(running, builder->initial_xmin_horizon);

		return true;
	}

	/*
	 * a) 没有事务在运行，可直接跳到一致。       
	 *
	 * 这不会受到 xl_running_xacts 周边竞争的影响，因为      
	 * 我们可能错过事务提交，但目前不会错过事务的   
	 *
	 * 注意：我们可能已开始增量地组装快照，因此需要     
	 * 小心地处理这种情况。             
	 */
	if (running->oldestRunningXid == running->nextXid)
	{
		if (builder->start_decoding_at == InvalidXLogRecPtr ||
			builder->start_decoding_at <= lsn)
			/* 此后一切均可解码         */
			builder->start_decoding_at = lsn + 1;

		/* 由于没有事务在运行，xmin/xmax 可直接简单设置   */
		builder->xmin = running->nextXid;	/* < 已完成    */
		builder->xmax = running->nextXid;	/* >= 在运行   */

		/* 因此可以安全地使用更快的比较  */
		Assert(TransactionIdIsNormal(builder->xmin));
		Assert(TransactionIdIsNormal(builder->xmax));

		builder->state = SNAPBUILD_CONSISTENT;
		builder->next_phase_at = InvalidTransactionId;

		ereport(LOG,
				(errmsg("logical decoding found consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("There are no running transactions.")));

		return false;
	}

	/*
	 * b) 磁盘状态有效，且既不在构建完整快照也未在    
	 * 创建槽。    
	 */
	else if (!builder->building_full_snapshot &&
			 !ctx->in_create &&
			 SnapBuildRestore(builder, lsn))
	{
		/* 不会有任何需要清理的状态*/
		return false;
	}

	/*
	 * c) 从 START 转换到 BUILDING_SNAPSHOT。   
	 *
	 * 处于 START 状态时，遇到带有在运行事务的            
	 * xl_running_xacts 记录时，切换到 BUILDING_SNAPSHOT 状态，
	 * 并记录 xl_running_xacts->nextXid。一旦全部在运行事务      
	 * 都已完成（即全部 >= nextXid），我们就有了完整         
	 * 的系统表快照。看起来我们可以利用 xl_running_xacts    
	 * 的 ->xids 信息更快达成目的，但这是有问题的，因为  
	 * 被标记为正在运行的事务可能已插入其提交记录   
	 * ，靠加锁改变这一点不可行。
	 */
	else if (builder->state == SNAPBUILD_START)
	{
		builder->state = SNAPBUILD_BUILDING_SNAPSHOT;
		builder->next_phase_at = running->nextXid;

		/*
		 * 从一个对将来正确的 xmin/xmax 开始，也就是所    
		 * 有当前运行的事务都完成时的值。在等待这些   
		 * 事务完成期间，我们会更新这两者。     
		 */
		builder->xmin = running->nextXid;	/* < 已完成    */
		builder->xmax = running->nextXid;	/* >= 在运行   */

		/* 因此可以安全地使用更快的比较  */
		Assert(TransactionIdIsNormal(builder->xmin));
		Assert(TransactionIdIsNormal(builder->xmax));

		ereport(LOG,
				(errmsg("logical decoding found initial starting point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("Waiting for transactions (approximately %d) older than %u to end.",
						   running->xcnt, running->nextXid)));

		SnapBuildWaitSnapshot(running, running->nextXid);
	}

	/*
	 * c) 从 BUILDING_SNAPSHOT 转换到 FULL_SNAPSHOT。   
	 *
	 * 处于 BUILDING_SNAPSHOT 状态，且该 xl_running_xacts 的          
	 * oldestRunningXid >= 切换到 BUILDING_SNAPSHOT 时的              
	 * nextXid。这意味着此后开始的事务都有足够信息可     
	 * 解码。切换到 FULL_SNAPSHOT。  
	 */
	else if (builder->state == SNAPBUILD_BUILDING_SNAPSHOT &&
			 TransactionIdPrecedesOrEquals(builder->next_phase_at,
										   running->oldestRunningXid))
	{
		builder->state = SNAPBUILD_FULL_SNAPSHOT;
		builder->next_phase_at = running->nextXid;

		ereport(LOG,
				(errmsg("logical decoding found initial consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("Waiting for transactions (approximately %d) older than %u to end.",
						   running->xcnt, running->nextXid)));

		SnapBuildWaitSnapshot(running, running->nextXid);
	}

	/*
	 * c) 从 FULL_SNAPSHOT 转换到 CONSISTENT。   
	 *
	 * 处于 FULL_SNAPSHOT 状态，且该 xl_running_xacts 的             
	 * oldestRunningXid >= 我们切换到 FULL_SNAPSHOT 时的 nextXid。这 
	 * 意味着当前所有在途的事务都有系统表快照，且它  
	 * 们的全部变更都已被收集。切换到 CONSISTENT。      
	 */
	else if (builder->state == SNAPBUILD_FULL_SNAPSHOT &&
			 TransactionIdPrecedesOrEquals(builder->next_phase_at,
										   running->oldestRunningXid))
	{
		builder->state = SNAPBUILD_CONSISTENT;
		builder->next_phase_at = InvalidTransactionId;

		ereport(LOG,
				(errmsg("logical decoding found consistent point at %X/%X",
						LSN_FORMAT_ARGS(lsn)),
				 errdetail("There are no old transactions anymore.")));
	}

	/*
	 * 我们已开始跟踪运行事务，需要等待所有在途的事
	 * 务完成。我们会落入记录的常规处理流程之中，以     
	 * 便可以执行增量清理。                  
	 */
	return true;
}

/* ---
 * 遍历记录中的 xid，等待所有早于 cutoff 的事务完      
 * 成。然后若可能，就记录一条新的 xl_running_xacts
 *
 * 这并非解码正确性所必需，但用于：            
 * a) 让 isolationtester 能够察觉我们正在等待某           
 *	  物。    
 * b) 在有帮助之处记录新的 xl_running_xacts 记录，而无需     
 *	  等待 bgwriter 或检查点进程。
 * ---
 */
static void
SnapBuildWaitSnapshot(xl_running_xacts *running, TransactionId cutoff)
{
	int			off;

	for (off = 0; off < running->xcnt; off++)
	{
		TransactionId xid = running->xids[off];

		/*
		 * 上层应当防止出现我们需要等待自身的情形。无    
		 * 论如何都检查一下，因为不这样做会导致无尽  
		 * 等待或 Assert() 失败。        
		 */
		if (TransactionIdIsCurrentTransactionId(xid))
			elog(ERROR, "waiting for ourselves");

		if (TransactionIdFollows(xid, cutoff))
			continue;

		XactLockTableWait(xid, NULL, NULL, XLTW_None);
	}

	/*
	 * 我们需要等待的所有事务都已完成——尽力确保及    
	 * 时出现另一条 xl_running_xacts 记录，而不必等待         
	 * bgwriter 或检查点进程来记录它。在恢复期间无法        
	 * 强制它，所以只能等待。   
	 */
	if (!RecoveryInProgress())
	{
		LogStandbySnapshot();
	}
}

/* -----------------------------------
 * 快照序列化支持         
 * -----------------------------------
 */

/*
 * 我们按以下方式把 struct SnapBuild 的当前状态存入磁盘：   
 *
 * struct SnapBuildOnDisk;
 * TransactionId * committed.xcnt; (*not xcnt_space*)
 * TransactionId * catchange.xcnt;
 *
 */
typedef struct SnapBuildOnDisk
{
	/* 该 struct 的第一部分需要与版本无关           */

	/* 校验和不覆盖的数据  */
	uint32		magic;
	pg_crc32c	checksum;

	/* 校验和覆盖的数据 */

	/* 版本号，以备将来支持 pg_upgrade      */
	uint32		version;
	/* 磁盘数据有多大，不含固定大小的部分              */
	uint32		length;

	/* 版本相关部分     */
	SnapBuild	builder;

	/* 后面跟有数量不定的 TransactionId */
} SnapBuildOnDisk;

#define SnapBuildOnDiskConstantSize \
	offsetof(SnapBuildOnDisk, builder)
#define SnapBuildOnDiskNotChecksummedSize \
	offsetof(SnapBuildOnDisk, version)

#define SNAPBUILD_MAGIC 0x51A1E001
#define SNAPBUILD_VERSION 5

/*
 * 根据快照构建器的状态，从磁盘存储/载入快照。           
 *
 * 供外部（即非 snapbuild.c）代码使用，前提是其刚读        
 * 取了一条记录，该位置可能存放有序列化快照。
 */
void
SnapBuildSerializationPoint(SnapBuild *builder, XLogRecPtr lsn)
{
	if (builder->state < SNAPBUILD_CONSISTENT)
		SnapBuildRestore(builder, lsn);
	else
		SnapBuildSerialize(builder, lsn);
}

/*
 * 若尚无其他解码进程做过此事，则在位置 'lsn' 处序        
 * 列化快照 'builder'。             
 */
static void
SnapBuildSerialize(SnapBuild *builder, XLogRecPtr lsn)
{
	Size		needed_length;
	SnapBuildOnDisk *ondisk = NULL;
	TransactionId *catchange_xip = NULL;
	MemoryContext old_ctx;
	size_t		catchange_xcnt;
	char	   *ondisk_c;
	int			fd;
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			ret;
	struct stat stat_buf;
	Size		sz;

	Assert(lsn != InvalidXLogRecPtr);
	Assert(builder->last_serialized_snapshot == InvalidXLogRecPtr ||
		   builder->last_serialized_snapshot <= lsn);

	/*
	 * 若恢复快照之后无法立即继续工作，那么序列化便     
	 * 毫无意义。       
	 */
	if (builder->state < SNAPBUILD_CONSISTENT)
		return;

	/* 一致的快照没有下一阶段       */
	Assert(builder->next_phase_at == InvalidTransactionId);

	/*
	 * 我们按快照在其上有效的 LSN 来标识快照。名称中    
	 * 无需包含时间线，因为每个 LSN 恰好映射到一条时     
	 * 间线，除非用户用过 pg_resetwal 或类似工具。若用户   
	 * 做过的话，反正解码无望。
	 */
	sprintf(path, "pg_logical/snapshots/%X-%X.snap",
			LSN_FORMAT_ARGS(lsn));

	/*
	 * 首先检查是否已有其他后端为该 LSN 写入了快照。      
	 * 若没有也完全正常，因此我们接受 ENOENT 作为有效     
	 * 状态。其他任何情况都是意外错误。         
	 */
	ret = stat(path, &stat_buf);

	if (ret != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));

	else if (ret == 0)
	{
		/*
		 * 他人已序列化到此，不要覆盖，只需记住该位置， 
		 * 这样就无需再次读取旧数据。                        
		 *
		 * 为确保在 rename() 把临时文件名改成真实文件名之 
		 * 后它已同步到磁盘，只需重复一次 fsync。这样做  
		 * 应当很廉价，因为大多数场景下它应当已经安全地
		 * 在磁盘上。   
		 */
		fsync_fname(path, false);
		fsync_fname("pg_logical/snapshots", true);

		builder->last_serialized_snapshot = lsn;
		goto out;
	}

	/*
	 * 在我们 stat(2) 该文件到写入该文件之间，存在明显     
	 * 的竞争条件。但我们会以原子方式重命名文件到
	 * 位，且所有已创建的文件反正都需要包含相同的数    
	 * 据，所以这完全没有问题，尽管有一点资源浪费。    
	 * 加锁似乎是无谓的复杂化 
	 */
	elog(DEBUG1, "serializing snapshot to %s", path);

	/* 为确保只有我们写入此临时文件，包含 pid       */
	sprintf(tmppath, "pg_logical/snapshots/%X-%X.snap.%d.tmp",
			LSN_FORMAT_ARGS(lsn), MyProcPid);

	/*
	 * 若临时文件已存在则将其解除链接；这必然发生        
	 * 在崩溃/错误之前，因为我们不会在单个解码槽/后 
	 * 端内两次进入该函数，且临时文件包含当前进程的     
	 * pid。              
	 */
	if (unlink(tmppath) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m", tmppath)));

	old_ctx = MemoryContextSwitchTo(builder->context);

	/* 获取尚未提交的修改系统表的事务                     */
	catchange_xip = ReorderBufferGetCatalogChangesXacts(builder->reorder);
	catchange_xcnt = dclist_count(&builder->reorder->catchange_txns);

	needed_length = sizeof(SnapBuildOnDisk) +
		sizeof(TransactionId) * (builder->committed.xcnt + catchange_xcnt);

	ondisk_c = palloc0(needed_length);
	ondisk = (SnapBuildOnDisk *) ondisk_c;
	ondisk->magic = SNAPBUILD_MAGIC;
	ondisk->version = SNAPBUILD_VERSION;
	ondisk->length = needed_length;
	INIT_CRC32C(ondisk->checksum);
	COMP_CRC32C(ondisk->checksum,
				((char *) ondisk) + SnapBuildOnDiskNotChecksummedSize,
				SnapBuildOnDiskConstantSize - SnapBuildOnDiskNotChecksummedSize);
	ondisk_c += sizeof(SnapBuildOnDisk);

	memcpy(&ondisk->builder, builder, sizeof(SnapBuild));
	/* 仅内存数据置 NULL   */
	ondisk->builder.context = NULL;
	ondisk->builder.snapshot = NULL;
	ondisk->builder.reorder = NULL;
	ondisk->builder.committed.xip = NULL;
	ondisk->builder.catchange.xip = NULL;
	/* 仅对磁盘数据更新 catchange */
	ondisk->builder.catchange.xcnt = catchange_xcnt;

	COMP_CRC32C(ondisk->checksum,
				&ondisk->builder,
				sizeof(SnapBuild));

	/* 复制已提交事务*/
	if (builder->committed.xcnt > 0)
	{
		sz = sizeof(TransactionId) * builder->committed.xcnt;
		memcpy(ondisk_c, builder->committed.xip, sz);
		COMP_CRC32C(ondisk->checksum, ondisk_c, sz);
		ondisk_c += sz;
	}

	/* 复制系统表修改事务  */
	if (catchange_xcnt > 0)
	{
		sz = sizeof(TransactionId) * catchange_xcnt;
		memcpy(ondisk_c, catchange_xip, sz);
		COMP_CRC32C(ondisk->checksum, ondisk_c, sz);
		ondisk_c += sz;
	}

	FIN_CRC32C(ondisk->checksum);

	/* 现在数据有效，打开临时文件并写入其中   */
	fd = OpenTransientFile(tmppath,
						   O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", tmppath)));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_WRITE);
	if ((write(fd, ondisk, needed_length)) != needed_length)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		/* 若 write 未设置 errno，则假定是磁盘空间不足 */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	/*
	 * 在重命名之前对文件执行 fsync，这样即使此后崩      
	 * 溃，要么完全有效，要么为空。
	 *
	 * 在这里直接因 fsync() 报 ERROR 是安全的，因为           
	 * 含写入在内都会重试。 
	 *
	 * TODO：改由检查点/重启点执行 fsync()？在这里执行     
	 * 有一些明显的开销，因为它在解码期间同步执行   
	 * ？      
	 */
	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_SYNC);
	if (pg_fsync(fd) != 0)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);
		errno = save_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	fsync_fname("pg_logical/snapshots", true);

	/*
	 * 我们可能覆盖其他某个后端已完成的工作，但这      
	 * 没关系，我们的快照同样有效，只是白做了些工作。 
	 */
	if (rename(tmppath, path) != 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));
	}

	/* 确保已持久化   */
	fsync_fname(path, false);
	fsync_fname("pg_logical/snapshots", true);

	/*
	 * 现在已不可能再丢失已转储的状态了，把这一点       
	 * 记为序列化点。    
	 */
	builder->last_serialized_snapshot = lsn;

	MemoryContextSwitchTo(old_ctx);

out:
	ReorderBufferSetRestartPoint(builder->reorder,
								 builder->last_serialized_snapshot);
	/* 清理  */
	if (ondisk)
		pfree(ondisk);
	if (catchange_xip)
		pfree(catchange_xip);
}

/*
 * 若之前在 'lsn' 指示的位置存储过快照，则将其恢          
 * 复到 'builder' 中。成功时返回 true，否则返回 false。      
 */
static bool
SnapBuildRestore(SnapBuild *builder, XLogRecPtr lsn)
{
	SnapBuildOnDisk ondisk;
	int			fd;
	char		path[MAXPGPATH];
	Size		sz;
	pg_crc32c	checksum;

	/* 若已处于该状态，则载入快照毫无意义   */
	if (builder->state == SNAPBUILD_CONSISTENT)
		return false;

	sprintf(path, "pg_logical/snapshots/%X-%X.snap",
			LSN_FORMAT_ARGS(lsn));

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);

	if (fd < 0 && errno == ENOENT)
		return false;
	else if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/* ----
	 * 确保快照已安全地存入磁盘，这一步通常很廉价       
	 * 。   
	 * 注意我们在此时不需要 PANIC：没有 fsync 就无人      
	 * 能使用该槽，而且不做 fsync() 保存也不会成功         
	 * ……   
	 * ----
	 */
	fsync_fname(path, false);
	fsync_fname("pg_logical/snapshots", true);


	/* 读取快照中固定大小的部分      */
	SnapBuildRestoreContents(fd, (char *) &ondisk, SnapBuildOnDiskConstantSize, path);

	if (ondisk.magic != SNAPBUILD_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("snapbuild state file \"%s\" has wrong magic number: %u instead of %u",
						path, ondisk.magic, SNAPBUILD_MAGIC)));

	if (ondisk.version != SNAPBUILD_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("snapbuild state file \"%s\" has unsupported version: %u instead of %u",
						path, ondisk.version, SNAPBUILD_VERSION)));

	INIT_CRC32C(checksum);
	COMP_CRC32C(checksum,
				((char *) &ondisk) + SnapBuildOnDiskNotChecksummedSize,
				SnapBuildOnDiskConstantSize - SnapBuildOnDiskNotChecksummedSize);

	/* 读 SnapBuild  */
	SnapBuildRestoreContents(fd, (char *) &ondisk.builder, sizeof(SnapBuild), path);
	COMP_CRC32C(checksum, &ondisk.builder, sizeof(SnapBuild));

	/* 恢复已提交事务的信息      */
	if (ondisk.builder.committed.xcnt > 0)
	{
		sz = sizeof(TransactionId) * ondisk.builder.committed.xcnt;
		ondisk.builder.committed.xip = MemoryContextAllocZero(builder->context, sz);
		SnapBuildRestoreContents(fd, (char *) ondisk.builder.committed.xip, sz, path);
		COMP_CRC32C(checksum, ondisk.builder.committed.xip, sz);
	}

	/* 恢复修改系统表事务的信息        */
	if (ondisk.builder.catchange.xcnt > 0)
	{
		sz = sizeof(TransactionId) * ondisk.builder.catchange.xcnt;
		ondisk.builder.catchange.xip = MemoryContextAllocZero(builder->context, sz);
		SnapBuildRestoreContents(fd, (char *) ondisk.builder.catchange.xip, sz, path);
		COMP_CRC32C(checksum, ondisk.builder.catchange.xip, sz);
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));

	FIN_CRC32C(checksum);

	/* 校验已读内容的校验和     */
	if (!EQ_CRC32C(checksum, ondisk.checksum))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum mismatch for snapbuild state file \"%s\": is %u, should be %u",
						path, checksum, ondisk.checksum)));

	/*
	 * 好，我们现在有了一个合理的快照，弄清它的信息 
	 * 是否比我们更多。 
	 */

	/*
	 * 我们目前只对一致的快照感兴趣；比较两个不完  
	 * 整的快照哪个更"先进"似乎不必要地复杂         
	 * 。                   
	 */
	if (ondisk.builder.state < SNAPBUILD_CONSISTENT)
		goto snapshot_not_interesting;

	/*
	 * 不要使用需要某个我们无法保证可用的 xmin 的          
	 * 快照。    
	 */
	if (TransactionIdPrecedes(ondisk.builder.xmin, builder->initial_xmin_horizon))
		goto snapshot_not_interesting;

	/*
	 * 一致的快照没有下一阶段。重置 next_phase_at，          
	 * 因为旧值可能残留下来。     
	 */
	Assert(ondisk.builder.next_phase_at == InvalidTransactionId);
	builder->next_phase_at = InvalidTransactionId;

	/* 好，我们认为该快照是合理的，复制所有重要内容    */
	builder->xmin = ondisk.builder.xmin;
	builder->xmax = ondisk.builder.xmax;
	builder->state = ondisk.builder.state;

	builder->committed.xcnt = ondisk.builder.committed.xcnt;
	/* 只分配/存储了 xcnt，而非 xcnt_space 个 xid */
	/* 若此处没有任何内容，则不要覆盖预分配的 xip    */
	if (builder->committed.xcnt > 0)
	{
		pfree(builder->committed.xip);
		builder->committed.xcnt_space = ondisk.builder.committed.xcnt;
		builder->committed.xip = ondisk.builder.committed.xip;
	}
	ondisk.builder.committed.xip = NULL;

	/* 设置修改系统表的事务     */
	if (builder->catchange.xip)
		pfree(builder->catchange.xip);
	builder->catchange.xcnt = ondisk.builder.catchange.xcnt;
	builder->catchange.xip = ondisk.builder.catchange.xip;
	ondisk.builder.catchange.xip = NULL;

	/* 我们的快照已不再有用，构建一个新的      */
	if (builder->snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(builder->snapshot);
	}
	builder->snapshot = SnapBuildBuildSnapshot(builder);
	SnapBuildSnapIncRefcount(builder->snapshot);

	ReorderBufferSetRestartPoint(builder->reorder, lsn);

	Assert(builder->state == SNAPBUILD_CONSISTENT);

	ereport(LOG,
			(errmsg("logical decoding found consistent point at %X/%X",
					LSN_FORMAT_ARGS(lsn)),
			 errdetail("Logical decoding will begin using saved snapshot.")));
	return true;

snapshot_not_interesting:
	if (ondisk.builder.committed.xip != NULL)
		pfree(ondisk.builder.committed.xip);
	if (ondisk.builder.catchange.xip != NULL)
		pfree(ondisk.builder.catchange.xip);
	return false;
}

/*
 * 把序列化快照的内容读取到 'dest' 中。     
 */
static void
SnapBuildRestoreContents(int fd, char *dest, Size size, const char *path)
{
	int			readBytes;

	pgstat_report_wait_start(WAIT_EVENT_SNAPBUILD_READ);
	readBytes = read(fd, dest, size);
	pgstat_report_wait_end();
	if (readBytes != size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		if (readBytes < 0)
		{
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", path)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, size)));
	}
}

/*
 * 移除所有不再需要的序列化快照，因为没有任何         
 * 槽会需要它们了。这其实并不一定必须在检查点期        
 * 间运行，但在此安排很方便。      
 *
 * NB: 即使逻辑解码被禁用，我们也在检查点期间运行       
 * 这项工作，以便在禁用后清理旧槽。         
 */
void
CheckPointSnapBuild(void)
{
	XLogRecPtr	cutoff;
	XLogRecPtr	redo;
	DIR		   *snap_dir;
	struct dirent *snap_de;
	char		path[MAXPGPATH + 21];

	/*
	 * 我们从最近的重做指针（最小值）起步。新的
	 * 复制槽不会在它之前启动，因此这是移除的安全      
	 * 上界。   
	 */
	redo = GetRedoRecPtr();

	/* 现在检查现有复制槽的 restart ptrs        */
	cutoff = ReplicationSlotsComputeLogicalRestartLSN();

	/* 不要早于 restart lsn 启动          */
	if (redo < cutoff)
		cutoff = redo;

	snap_dir = AllocateDir("pg_logical/snapshots");
	while ((snap_de = ReadDir(snap_dir, "pg_logical/snapshots")) != NULL)
	{
		uint32		hi;
		uint32		lo;
		XLogRecPtr	lsn;
		PGFileType	de_type;

		if (strcmp(snap_de->d_name, ".") == 0 ||
			strcmp(snap_de->d_name, "..") == 0)
			continue;

		snprintf(path, sizeof(path), "pg_logical/snapshots/%s", snap_de->d_name);
		de_type = get_dirent_type(path, snap_de, false, DEBUG1);

		if (de_type != PGFILETYPE_ERROR && de_type != PGFILETYPE_REG)
		{
			elog(DEBUG1, "only regular files expected: %s", path);
			continue;
		}

		/*
		 * SnapBuildSerialize() 的临时文件名包含 LSN 以及其      
		 * 他信息，但以 .$pid.tmp 作为后缀。可以像其他文件 
		 * 一样直接移除，因为正在写的文件不会早于 
		 * cutoff。                                          
		 *
		 * 若文件不符合该模式，只记录一条消息即可， 
		 * 可能是编辑器的锁/状态文件之类……  
		 */
		if (sscanf(snap_de->d_name, "%X-%X.snap", &hi, &lo) != 2)
		{
			ereport(LOG,
					(errmsg("could not parse file name \"%s\"", path)));
			continue;
		}

		lsn = ((uint64) hi) << 32 | lo;

		/* 检查是否仍然需要它    */
		if (lsn < cutoff || cutoff == InvalidXLogRecPtr)
		{
			elog(DEBUG1, "removing snapbuild snapshot %s", path);

			/*
			 * 无法在此移除该文件虽奇怪，但害处不大。 
			 * 不要阻止检查点完成，疗法比疾病更糟    
			 * 。                                                 
			 */
			if (unlink(path) < 0)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m",
								path)));
				continue;
			}
		}
	}
	FreeDir(snap_dir);
}
