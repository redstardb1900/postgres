/*-------------------------------------------------------------------------
 *
 * reorderbuffer.c
 *	  PostgreSQL 逻辑重放/重排序缓冲区管理   
 *
 *
 * Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/reorderbuffer.c
 *
 * NOTES
 *	  本模块按事务写入 WAL 的顺序接收事务分片，并负责  
 *	  将其重新拼装为顶层事务大小的分片。当某个事务    
 *	  被完全拼装好（即读到提交记录）后就会带着变更 
 *	  调用输出插件（参见 ReorderBufferCommit()），并传给它 
 *	  各个变更。输出插件依赖于由 snapbuild.c 构建并        
 *	  由其交给我们的                                            
 *	  快照。                          
 *
 *	  postgres 中事务与子事务/保存点在执行后端之外  
 *	  并不立即相互关联。只有在提交/中止（或     
 *	  xact_assignment 记录）时才链接到一起。这意味着我们    
 *	  必须从其子事务中拼接出顶层事务。为了高效做到     
 *	  这一点，我们构建一个二叉堆，以各个子事务            
 *	  变更流的当前最小 LSN 为索引。由于各个流本身           
 *	  就按 LSN 有序——毕竟我们就是依据它来构建的——    
 *	  所以事务很容易通过始终选用堆中当前 LSN 最小           
 *	  的子事务来重新拼装                                          
 *	  完成。                          
 *
 *	  为了应对可能数倍于可用内存大小的大事务——         
 *	  本模块支持将大事务内容溢写到磁盘。重放时，         
 *	  各个（子）事务的内容将                                    
 *	  以块为单位从磁盘读回。                                  
 *	         
 *
 *	  本模块还必须重新拼装 TOAST 记录，它由 WAL 中         
 *	  存储的各个块组成。当元组的新（或初始）版本被   
 *	  存入 WAL 时，其前面总会先有针对行外存储的列所    
 *	  发出的 TOAST 块。在单个顶层事务中，某行的 TOAST    
 *	  块与该行数据本身之间不会有其他承载数据的记录。   
 *	  详情参见 ReorderBufferToast*。                              
 *	          
 *
 *	  ReorderBuffer 使用两种特殊的内存上下文类型——       
 *	  SlabContext 用于分配固定长度结构（变更与事务），    
 *	  GenerationContext 用于可变长度的事务数据（按生命周   
 *	  期相近的组进行分配与释放）。  
 *
 *	  为限制已解码变更占用的内存量，我们在重排序缓冲 
 *	  区层级（即总内存）与每个事务上跟踪内存使用量。  
 *	  当已用内存总量超上限时，消耗最多的事务         
 *	  将被序列化到磁盘上。                                        
 *	       
 *
 *	  内存中只逐出已解码变更（溢写到磁盘），而不逐出  
 *	  事务记录。顶层事务数量虽有限，                       
 *	  但拥有大量子事务的单个事务仍可能占用可观的内存。 
 *	  不过，事务记录相当小，并不计入内存上限之内。      
 *	                                       
 *
 *	  当前逐出算法很简单：仅按大小挑选事务，而      
 *	  例如或许也值得考虑变更的年龄（LSN）。借助        
 *	  Generational 内存分配器，逐出最旧变更更有可能     
 *	  让内存真正得到释放。                                      
 *	                             
 *
 *	  在把已序列化的变更载回内存时，我们仍依赖          
 *	  max_changes_in_memory。此时我们无法直接使用内存上       
 *	  限，因为子事务是独立载入的。                         
 *	  一种做法是统计子事务数量，并让各自分配 1/N 的    
 *	  内存上限。但这似乎不太有吸引力，子事务多时极   
 *	  易颠簸（反序列化并应用极少变更的循环）。我们 
 *	  或许应给最旧的子事务多一些内存，因为它们很可   
 *	  能是下一批变更的来源。                                   
 *	                                                       
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/detoast.h"
#include "access/heapam.h"
#include "access/rewriteheap.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "catalog/catalog.h"
#include "lib/binaryheap.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/reorderbuffer.h"
#include "replication/slot.h"
#include "replication/snapbuild.h"	/* 仅供 SnapBuildSnapDecRefcount   */
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/inval.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relfilenumbermap.h"

/*
 * 每个事务对由其他事务分发来的失效消息有 8MB 的上限。  
 * 此上限是考虑到大量并发逻辑解码场景而设定的。   
 * 当分发的失效消息达到这一阈值时，该事务将被标记    
 * 记为 RBTXN_DISTR_INVAL_OVERFLOWED，以便使整         
 * 个缓存失效——因为我们已经丢失了一些失效消息，        
 * 从而不知道需要失效哪些内容。                            
 */
#define MAX_DISTR_INVAL_MSG_PER_TXN \
	((8 * 1024 * 1024) / sizeof(SharedInvalidationMessage))

/* 哈希表条目，用于从 xid 映射到我们的事务状态         */
typedef struct ReorderBufferTXNByIdEnt
{
	TransactionId xid;
	ReorderBufferTXN *txn;
} ReorderBufferTXNByIdEnt;

/* (relfilelocator, ctid) => (cmin, cmax) 映射的数据结构       */
typedef struct ReorderBufferTupleCidKey
{
	RelFileLocator rlocator;
	ItemPointerData tid;
} ReorderBufferTupleCidKey;

typedef struct ReorderBufferTupleCidEnt
{
	ReorderBufferTupleCidKey key;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* 仅用于调试    */
} ReorderBufferTupleCidEnt;

/* 带文件偏移量跟踪的虚拟文件描述符  */
typedef struct TXNEntryFile
{
	File		vfd;			/* 文件关闭时为 -1      */
	off_t		curOffset;		/* offset for next write or read. Reset to 0
								 * when vfd is opened. */
} TXNEntryFile;

/* k 路按序变更迭代的支持结构             */
typedef struct ReorderBufferIterTXNEntry
{
	XLogRecPtr	lsn;
	ReorderBufferChange *change;
	ReorderBufferTXN *txn;
	TXNEntryFile file;
	XLogSegNo	segno;
} ReorderBufferIterTXNEntry;

typedef struct ReorderBufferIterTXNState
{
	binaryheap *heap;
	Size		nr_txns;
	dlist_head	old_change;
	ReorderBufferIterTXNEntry entries[FLEXIBLE_ARRAY_MEMBER];
} ReorderBufferIterTXNState;

/* TOAST 数据结构   */
typedef struct ReorderBufferToastEnt
{
	Oid			chunk_id;		/* toast_table.chunk_id */
	int32		last_chunk_seq; /* toast_table.chunk_seq of the last chunk we
								 * have seen */
	Size		num_chunks;		/* 我们已见过的块的数量      */
	Size		size;			/* 已见块的合计大小     */
	dlist_head	chunks;			/* 块的链表          */
	struct varlena *reconstructed;	/* reconstructed varlena now pointed to in
									 * main tup */
} ReorderBufferToastEnt;

/* 磁盘序列化支持数据结构         */
typedef struct ReorderBufferDiskChange
{
	Size		size;
	ReorderBufferChange change;
	/* 数据如下 */
} ReorderBufferDiskChange;

#define IsSpecInsert(action) \
( \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT) \
)
#define IsSpecConfirmOrAbort(action) \
( \
	(((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM) || \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT)) \
)
#define IsInsertOrUpdate(action) \
( \
	(((action) == REORDER_BUFFER_CHANGE_INSERT) || \
	((action) == REORDER_BUFFER_CHANGE_UPDATE) || \
	((action) == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT)) \
)

/*
 * 每个事务在内存中保留的最大变更数量。超过后，变 
 * 更将溢写到磁盘。    
 *
 * 当前值应足以在 OLTP 负载下解码整个事务而不触及磁  
 * 盘，同时在其他负载下也较快开始溢写到磁盘。           
 *                                 
 *
 * 将来某个时候，这里或许应该有更精细的资源管理机制， 
 * 但目前并不完全清楚那会是什么样子。                       
 *      
 */
int			logical_decoding_work_mem;
static const Size max_changes_in_memory = 4096; /* XXX 仅用于恢复  */

/* GUC 变量   */
int			debug_logical_replication_streaming = DEBUG_LOGICAL_REP_STREAMING_BUFFERED;

/* ---------------------------------------
 * 重排序缓冲区主支持例程     
 * ---------------------------------------
 */
static ReorderBufferTXN *ReorderBufferGetTXN(ReorderBuffer *rb);
static void ReorderBufferReturnTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static ReorderBufferTXN *ReorderBufferTXNByXid(ReorderBuffer *rb,
											   TransactionId xid, bool create, bool *is_new,
											   XLogRecPtr lsn, bool create_as_top);
static void ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
											  ReorderBufferTXN *subtxn);

static void AssertTXNLsnOrder(ReorderBuffer *rb);

/* ---------------------------------------
 * 按 LSN 顺序遍历某个事务及其子事务的 ->changes 的 
 * 支持函数                       
 *
 * 用于对一个事务及其子事务的 k 路堆合并               
 * 进行遍历   
 * ---------------------------------------
 */
static void ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
									 ReorderBufferIterTXNState *volatile *iter_state);
static ReorderBufferChange *ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state);
static void ReorderBufferIterTXNFinish(ReorderBuffer *rb,
									   ReorderBufferIterTXNState *state);
static void ReorderBufferExecuteInvalidations(uint32 nmsgs, SharedInvalidationMessage *msgs);

/*
 * ---------------------------------------
 * 磁盘序列化支持函数         
 * ---------------------------------------
 */
static void ReorderBufferCheckMemoryLimit(ReorderBuffer *rb);
static void ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
										 int fd, ReorderBufferChange *change);
static Size ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
										TXNEntryFile *file, XLogSegNo *segno);
static void ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
									   char *data);
static void ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferTruncateTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
									 bool txn_prepared);
static void ReorderBufferCleanupSerializedTXNs(const char *slotname);
static void ReorderBufferSerializedPath(char *path, ReplicationSlot *slot,
										TransactionId xid, XLogSegNo segno);

static void ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap);
static Snapshot ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
									  ReorderBufferTXN *txn, CommandId cid);

/*
 * ---------------------------------------
 * 流式支持函数         
 * ---------------------------------------
 */
static inline bool ReorderBufferCanStream(ReorderBuffer *rb);
static inline bool ReorderBufferCanStartStreaming(ReorderBuffer *rb);
static void ReorderBufferStreamTXN(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferStreamCommit(ReorderBuffer *rb, ReorderBufferTXN *txn);

/* ---------------------------------------
 * TOAST 重组支持      
 * ---------------------------------------
 */
static void ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn);
static void ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
									  Relation relation, ReorderBufferChange *change);
static void ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
										  Relation relation, ReorderBufferChange *change);

/*
 * ---------------------------------------
 * 内存统计     
 * ---------------------------------------
 */
static Size ReorderBufferChangeSize(ReorderBufferChange *change);
static void ReorderBufferChangeMemoryUpdate(ReorderBuffer *rb,
											ReorderBufferChange *change,
											bool addition, Size sz);

/*
 * 分配一个新的 ReorderBuffer，并清除同一槽上前几个       
 * ReorderBuffer 的旧序列化状态。          
 */
ReorderBuffer *
ReorderBufferAllocate(void)
{
	ReorderBuffer *buffer;
	HASHCTL		hash_ctl;
	MemoryContext new_ctx;

	Assert(MyReplicationSlot != NULL);

	/* 在自己的上下文中分配内存，便于更好地追踪  */
	new_ctx = AllocSetContextCreate(CurrentMemoryContext,
									"ReorderBuffer",
									ALLOCSET_DEFAULT_SIZES);

	buffer =
		(ReorderBuffer *) MemoryContextAlloc(new_ctx, sizeof(ReorderBuffer));

	memset(&hash_ctl, 0, sizeof(hash_ctl));

	buffer->context = new_ctx;

	buffer->change_context = SlabContextCreate(new_ctx,
											   "Change",
											   SLAB_DEFAULT_BLOCK_SIZE,
											   sizeof(ReorderBufferChange));

	buffer->txn_context = SlabContextCreate(new_ctx,
											"TXN",
											SLAB_DEFAULT_BLOCK_SIZE,
											sizeof(ReorderBufferTXN));

	/*
	 * 为了减少变更跨越多个内存块的长事务导致的内存  
	 * 碎片，我们为已解码元组使用单一固定大小的 
	 * 内存块。测试表明，默认的内存块大小可维持      
	 * 逻辑解码性能，也不会因并发事务产生碎片。可能 
	 * 会认为可以把最大尺寸用作                                
	 * SLAB_LARGE_BLOCK_SIZE，但测试同样表明它无助于解 
	 * 决内存碎片。                                                    
	 *                          
	 */
	buffer->tup_context = GenerationContextCreate(new_ctx,
												  "Tuples",
												  SLAB_DEFAULT_BLOCK_SIZE,
												  SLAB_DEFAULT_BLOCK_SIZE,
												  SLAB_DEFAULT_BLOCK_SIZE);

	hash_ctl.keysize = sizeof(TransactionId);
	hash_ctl.entrysize = sizeof(ReorderBufferTXNByIdEnt);
	hash_ctl.hcxt = buffer->context;

	buffer->by_txn = hash_create("ReorderBufferByXid", 1000, &hash_ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	buffer->by_txn_last_xid = InvalidTransactionId;
	buffer->by_txn_last_txn = NULL;

	buffer->outbuf = NULL;
	buffer->outbufsize = 0;
	buffer->size = 0;

	buffer->spillTxns = 0;
	buffer->spillCount = 0;
	buffer->spillBytes = 0;
	buffer->streamTxns = 0;
	buffer->streamCount = 0;
	buffer->streamBytes = 0;
	buffer->totalTxns = 0;
	buffer->totalBytes = 0;

	buffer->current_restart_decoding_lsn = InvalidXLogRecPtr;

	dlist_init(&buffer->toplevel_by_lsn);
	dlist_init(&buffer->txns_by_base_snapshot_lsn);
	dclist_init(&buffer->catchange_txns);

	/*
	 * 确保此槽没有上次使用遗留的陈旧数据，以免上一次  
	 * 退出路径未调用 ReorderBufferFree。不这样做会产生重  
	 * 复的事务；如果槽中没有内容，其代价也非常低。    
	 */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));

	return buffer;
}

/*
 * 释放ReorderBuffer 
 */
void
ReorderBufferFree(ReorderBuffer *rb)
{
	MemoryContext context = rb->context;

	/*
	 * 废弃重排序缓冲区内存上下文即可释放单独分配的     
	 * 数据。      
	 */
	MemoryContextDelete(context);

	/* 释放未消费重排序缓冲区的磁盘空间   */
	ReorderBufferCleanupSerializedTXNs(NameStr(MyReplicationSlot->data.name));
}

/*
 * 取得空闲（或预分配）的 ReorderBufferTXN。  
 */
static ReorderBufferTXN *
ReorderBufferGetTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	txn = (ReorderBufferTXN *)
		MemoryContextAlloc(rb->txn_context, sizeof(ReorderBufferTXN));

	memset(txn, 0, sizeof(ReorderBufferTXN));

	dlist_init(&txn->changes);
	dlist_init(&txn->tuplecids);
	dlist_init(&txn->subtxns);

	/* InvalidCommandId 不为零，因此要显式设置 */
	txn->command_id = InvalidCommandId;
	txn->output_plugin_private = NULL;

	return txn;
}

/*
 * 释放 ReorderBufferTXN 
 */
static void
ReorderBufferReturnTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* 若被缓存（很可能），则清理查找缓存     */
	if (rb->by_txn_last_xid == txn->xid)
	{
		rb->by_txn_last_xid = InvalidTransactionId;
		rb->by_txn_last_txn = NULL;
	}

	/* 释放所包含的数据   */

	if (txn->gid != NULL)
	{
		pfree(txn->gid);
		txn->gid = NULL;
	}

	if (txn->tuplecid_hash != NULL)
	{
		hash_destroy(txn->tuplecid_hash);
		txn->tuplecid_hash = NULL;
	}

	if (txn->invalidations)
	{
		pfree(txn->invalidations);
		txn->invalidations = NULL;
	}

	if (txn->invalidations_distributed)
	{
		pfree(txn->invalidations_distributed);
		txn->invalidations_distributed = NULL;
	}

	/* 重置 TOAST 哈希  */
	ReorderBufferToastReset(rb, txn);

	pfree(txn);
}

/*
 * 获得 ReorderBufferChange。   
 */
ReorderBufferChange *
ReorderBufferGetChange(ReorderBuffer *rb)
{
	ReorderBufferChange *change;

	change = (ReorderBufferChange *)
		MemoryContextAlloc(rb->change_context, sizeof(ReorderBufferChange));

	memset(change, 0, sizeof(ReorderBufferChange));
	return change;
}

/*
 * 释放一个 ReorderBufferChange，并按需更新内存统计。     
 */
void
ReorderBufferReturnChange(ReorderBuffer *rb, ReorderBufferChange *change,
						  bool upd_mem)
{
	/* 更新内存统计信息      */
	if (upd_mem)
		ReorderBufferChangeMemoryUpdate(rb, change, false,
										ReorderBufferChangeSize(change));

	/* 释放所含数据  */
	switch (change->action)
	{
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.newtuple)
			{
				ReorderBufferReturnTupleBuf(rb, change->data.tp.newtuple);
				change->data.tp.newtuple = NULL;
			}

			if (change->data.tp.oldtuple)
			{
				ReorderBufferReturnTupleBuf(rb, change->data.tp.oldtuple);
				change->data.tp.oldtuple = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			if (change->data.msg.prefix != NULL)
				pfree(change->data.msg.prefix);
			change->data.msg.prefix = NULL;
			if (change->data.msg.message != NULL)
				pfree(change->data.msg.message);
			change->data.msg.message = NULL;
			break;
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			if (change->data.inval.invalidations)
				pfree(change->data.inval.invalidations);
			change->data.inval.invalidations = NULL;
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			if (change->data.snapshot)
			{
				ReorderBufferFreeSnap(rb, change->data.snapshot);
				change->data.snapshot = NULL;
			}
			break;
			/* 除结构体本身外没有其他数据  */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			if (change->data.truncate.relids != NULL)
			{
				ReorderBufferReturnRelids(rb, change->data.truncate.relids);
				change->data.truncate.relids = NULL;
			}
			break;
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	pfree(change);
}

/*
 * 获取一个新的 ReorderBufferTupleBuf，至少能装下大小  
 * 为 tuple_len（不含头部）。    
 */
ReorderBufferTupleBuf *
ReorderBufferGetTupleBuf(ReorderBuffer *rb, Size tuple_len)
{
	ReorderBufferTupleBuf *tuple;
	Size		alloc_len;

	alloc_len = tuple_len + SizeofHeapTupleHeader;

	tuple = (ReorderBufferTupleBuf *)
		MemoryContextAlloc(rb->tup_context,
						   sizeof(ReorderBufferTupleBuf) +
						   MAXIMUM_ALIGNOF + alloc_len);
	tuple->alloc_tuple_size = alloc_len;
	tuple->tuple.t_data = ReorderBufferTupleBufData(tuple);

	return tuple;
}

/*
 * 释放 ReorderBufferTupleBuf 
 */
void
ReorderBufferReturnTupleBuf(ReorderBuffer *rb, ReorderBufferTupleBuf *tuple)
{
	pfree(tuple);
}

/*
 * 获取已清截关系的 relid 数组。       
 *
 * 我们使用全局内存上下文（整个重排序缓冲区的），   
 * 因为现有上下文看起来都不合适（有些是 SLAB，所以   
 * 无法使用；tup_context 则用于元组数据，而非                
 * relid）。我们本可以再加一个上下文，但那似乎有点      
 * 大材小用——TRUNCATE 并不常见，似乎不值得。    
 */
Oid *
ReorderBufferGetRelids(ReorderBuffer *rb, int nrelids)
{
	Oid		   *relids;
	Size		alloc_len;

	alloc_len = sizeof(Oid) * nrelids;

	relids = (Oid *) MemoryContextAlloc(rb->context, alloc_len);

	return relids;
}

/*
 * 释放 relid 数组。  
 */
void
ReorderBufferReturnRelids(ReorderBuffer *rb, Oid *relids)
{
	pfree(relids);
}

/*
 * 从给定缓冲区中返回由 Xid 指定的 ReorderBufferTXN。    
 * 若 create 为真且事务尚不存在，则创建它（以给定的 
 * LSN，如指定则作为顶层事务）；此时，is_new 会被 
 * 设置为 true。                        
 */
static ReorderBufferTXN *
ReorderBufferTXNByXid(ReorderBuffer *rb, TransactionId xid, bool create,
					  bool *is_new, XLogRecPtr lsn, bool create_as_top)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXNByIdEnt *ent;
	bool		found;

	Assert(TransactionIdIsValid(xid));

	/*
	 * 首先检查单条目查找缓存     
	 */
	if (TransactionIdIsValid(rb->by_txn_last_xid) &&
		rb->by_txn_last_xid == xid)
	{
		txn = rb->by_txn_last_txn;

		if (txn != NULL)
		{
			/* 找到了且其有效    */
			if (is_new)
				*is_new = false;
			return txn;
		}

		/*
		 * 已缓存为不存在且被要求不创建？那就无事可做   
		 *       
		 */
		if (!create)
			return NULL;
		/* 否则继续往下执行以创建它*/
	}

	/*
	 * 如果缓存未命中，或者命中结果为不存在且我们想     
	 * 创建条目。 
	 */

	/* 搜索查找表         */
	ent = (ReorderBufferTXNByIdEnt *)
		hash_search(rb->by_txn,
					&xid,
					create ? HASH_ENTER : HASH_FIND,
					&found);
	if (found)
		txn = ent->txn;
	else if (create)
	{
		/* 如果请求了创建，则初始化新条目       */
		Assert(ent != NULL);
		Assert(lsn != InvalidXLogRecPtr);

		ent->txn = ReorderBufferGetTXN(rb);
		ent->txn->xid = xid;
		txn = ent->txn;
		txn->first_lsn = lsn;
		txn->restart_decoding_lsn = rb->current_restart_decoding_lsn;

		if (create_as_top)
		{
			dlist_push_tail(&rb->toplevel_by_lsn, &txn->node);
			AssertTXNLsnOrder(rb);
		}
	}
	else
		txn = NULL;				/* 未找到且未被要求创建    */

	/* 更新缓存 */
	rb->by_txn_last_xid = xid;
	rb->by_txn_last_txn = txn;

	if (is_new)
		*is_new = !found;

	Assert(!create || txn != NULL);
	return txn;
}

/*
 * 我们会为进行中事务的流式发送记录部分变更。我们       
 * 只能流式发送完整的变更，因此若存在部分变更（例如  
 * TOAST 表插入或推测性插入），则标记该事务，使其     
 * 无法被流式发送。我们还确保，若该事务中的变更可以  
 * 被流式发送且其大小超过 logical_decoding_work_mem 阈值，       
 * 则一有完整变更即流式发送。   
 */
static void
ReorderBufferProcessPartialChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
								  ReorderBufferChange *change,
								  bool toast_insert)
{
	ReorderBufferTXN *toptxn;

	/*
	 * 只在流式发送进行中事务时，才需要处理这些 
	 * 部分变更。          
	 */
	if (!ReorderBufferCanStream(rb))
		return;

	/* 获取顶层事务。    */
	toptxn = rbtxn_get_toptxn(txn);

	/*
	 * 为 TOAST 插入指示一个部分变更。该变更会在      
	 * 我们获得主表上的插入或更新、且确定待处理的    
	 * TOAST 块不再需要之后被认定为完整。                    
	 *         
	 *
	 * 若存在待处理的 TOAST 块时仍允许流式发送，那么   
	 * 这些块要等到插入（multi_insert）完成后才会释放，     
	 * 并预期该事务在流式发送后已流式发送全部变更。   
	 * 该限制主要为确保流式事务的正确性，        
	 * 似乎不值得仅为支持该情况而放开这一限制，       
	 * 因为反正该事务都会在插入完成后被流式发送。      
	 *                                 
	 */
	if (toast_insert)
		toptxn->txn_flags |= RBTXN_HAS_PARTIAL_CHANGE;
	else if (rbtxn_has_partial_change(toptxn) &&
			 IsInsertOrUpdate(change->action) &&
			 change->data.tp.clear_toast_afterwards)
		toptxn->txn_flags &= ~RBTXN_HAS_PARTIAL_CHANGE;

	/*
	 * 为推测性插入指示一个部分变更。该变更会在我们    
	 * 获得推测确认或中止词元后被认定为完整。          
	 *       
	 */
	if (IsSpecInsert(change->action))
		toptxn->txn_flags |= RBTXN_HAS_PARTIAL_CHANGE;
	else if (rbtxn_has_partial_change(toptxn) &&
			 IsSpecConfirmOrAbort(change->action))
		toptxn->txn_flags &= ~RBTXN_HAS_PARTIAL_CHANGE;

	/*
	 * 如果事务之前已被序列化，且变更现在已在顶层事   
	 * 务中已完整，则流式发送它。   
	 *
	 * 之所以一拿到该事务的完整变更就立即对其进行流式 
	 * 发送，原因在于此前它会达到内存阈值，但因存在     
	 * 不完整变更而无法流式发送。推迟这类事务的流式  
	 * 发送会加大它们的应用滞后。                                
	 */
	if (ReorderBufferCanStartStreaming(rb) &&
		!(rbtxn_has_partial_change(toptxn)) &&
		rbtxn_is_serialized(txn) &&
		rbtxn_has_streamable_change(toptxn))
		ReorderBufferStreamTXN(rb, toptxn);
}

/*
 * 把一个变更排入某个事务的队列，使其在提交时被重放，   
 * 或在 logical_decoding_work_mem 阈值时流式发送。  
 */
void
ReorderBufferQueueChange(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
						 ReorderBufferChange *change, bool toast_insert)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	/*
	 * 在流式发送先前变更时已检测到该事务中止。  
	 * 因此再为它收集更多变更已没有意义。                
	 *                
	 */
	if (txn->concurrent_abort)
	{
		/*
		 * 我们无需为该变更更新内存统计，因为            
		 * 尚未将其加入队列。        
		 */
		ReorderBufferReturnChange(rb, change, false);
		return;
	}

	/*
	 * 已发送到下游的变更被视为可流式发送的变更。    
	 * 我们会记住这些事务，之后仅将它们纳入流式发送    
	 * 的考虑。  
	 */
	if (change->action == REORDER_BUFFER_CHANGE_INSERT ||
		change->action == REORDER_BUFFER_CHANGE_UPDATE ||
		change->action == REORDER_BUFFER_CHANGE_DELETE ||
		change->action == REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT ||
		change->action == REORDER_BUFFER_CHANGE_TRUNCATE ||
		change->action == REORDER_BUFFER_CHANGE_MESSAGE)
	{
		ReorderBufferTXN *toptxn = rbtxn_get_toptxn(txn);

		toptxn->txn_flags |= RBTXN_HAS_STREAMABLE_CHANGE;
	}

	change->lsn = lsn;
	change->txn = txn;

	Assert(InvalidXLogRecPtr != lsn);
	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries++;
	txn->nentries_mem++;

	/* 更新内存统计信息             */
	ReorderBufferChangeMemoryUpdate(rb, change, true,
									ReorderBufferChangeSize(change));

	/* 处理部分变更     */
	ReorderBufferProcessPartialChange(rb, txn, change, toast_insert);

	/* 检查内存上限，必要时逐出一些内容      */
	ReorderBufferCheckMemoryLimit(rb);
}

/*
 * 事务性消息会进入队列，等提交时再处理；          
 * 非事务性消息则立即处理。                 
 */
void
ReorderBufferQueueMessage(ReorderBuffer *rb, TransactionId xid,
						  Snapshot snap, XLogRecPtr lsn,
						  bool transactional, const char *prefix,
						  Size message_size, const char *message)
{
	if (transactional)
	{
		MemoryContext oldcontext;
		ReorderBufferChange *change;

		Assert(xid != InvalidTransactionId);

		/*
		 * 我们不预期事务性变更会带快照——将使用稍后    
		 * 应用时推导的快照（除非该变更被跳过）。  
		 *          
		 */
		Assert(!snap);

		oldcontext = MemoryContextSwitchTo(rb->context);

		change = ReorderBufferGetChange(rb);
		change->action = REORDER_BUFFER_CHANGE_MESSAGE;
		change->data.msg.prefix = pstrdup(prefix);
		change->data.msg.message_size = message_size;
		change->data.msg.message = palloc(message_size);
		memcpy(change->data.msg.message, message, message_size);

		ReorderBufferQueueChange(rb, xid, lsn, change, false);

		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		ReorderBufferTXN *txn = NULL;
		volatile Snapshot snapshot_now = snap;

		/* 非事务性变更需要有效的快照。          */
		Assert(snapshot_now);

		if (xid != InvalidTransactionId)
			txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

		/* 设置快照以允许访问系统表   */
		SetupHistoricSnapshot(snapshot_now, NULL);
		PG_TRY();
		{
			rb->message(rb, txn, lsn, false, prefix, message_size, message);

			TeardownHistoricSnapshot(false);
		}
		PG_CATCH();
		{
			TeardownHistoricSnapshot(true);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
}

/*
 * AssertTXNLsnOrder
 *		校验重排序缓冲区中事务列表的 LSN 顺序        
 *
 * 同时检查其他 LSN 相关的不变式。 
 *
 * 未启用断言时为空操作。  
 */
static void
AssertTXNLsnOrder(ReorderBuffer *rb)
{
#ifdef USE_ASSERT_CHECKING
	LogicalDecodingContext *ctx = rb->private_data;
	dlist_iter	iter;
	XLogRecPtr	prev_first_lsn = InvalidXLogRecPtr;
	XLogRecPtr	prev_base_snap_lsn = InvalidXLogRecPtr;

	/*
	 * 若尚未到达开始解码事务内容的 LSN，则跳过该    
	 * 校验，因为在到达该 LSN 之前，可能存在顶层事务   
	 * 与子事务之间尚未建立关联、因而具有相同 LSN 的      
	 * 事务。只有在尝试解码事务的实际内容时，我们才    
	 * 能保证这种关联。                                          
	 * start_decoding_at LSN 之前的记录顺序应当在重启之         
	 * 前就已检查过。                                                 
	 *         
	 */
	if (SnapBuildXactNeedsSkip(ctx->snapshot_builder, ctx->reader->EndRecPtr))
		return;

	dlist_foreach(iter, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN, node,
													iter.cur);

		/* 必须设置起始 LSN*/
		Assert(cur_txn->first_lsn != InvalidXLogRecPtr);

		/* 如果有结束 LSN，它必须高于起始 LSN          */
		if (cur_txn->end_lsn != InvalidXLogRecPtr)
			Assert(cur_txn->first_lsn <= cur_txn->end_lsn);

		/* 当前初始 LSN 必须严格高于前一个              */
		if (prev_first_lsn != InvalidXLogRecPtr)
			Assert(prev_first_lsn < cur_txn->first_lsn);

		/* 已知为子事务的事务不许列出 */
		Assert(!rbtxn_is_known_subxact(cur_txn));

		prev_first_lsn = cur_txn->first_lsn;
	}

	dlist_foreach(iter, &rb->txns_by_base_snapshot_lsn)
	{
		ReorderBufferTXN *cur_txn = dlist_container(ReorderBufferTXN,
													base_snapshot_node,
													iter.cur);

		/* 基础快照（及其 LSN）必须设置*/
		Assert(cur_txn->base_snapshot != NULL);
		Assert(cur_txn->base_snapshot_lsn != InvalidXLogRecPtr);

		/* 当前 LSN 必须严格高于前一个            */
		if (prev_base_snap_lsn != InvalidXLogRecPtr)
			Assert(prev_base_snap_lsn < cur_txn->base_snapshot_lsn);

		/* 已知为子事务的事务不许列出 */
		Assert(!rbtxn_is_known_subxact(cur_txn));

		prev_base_snap_lsn = cur_txn->base_snapshot_lsn;
	}
#endif
}

/*
 * AssertChangeLsnOrder
 *
 * 检查（子）事务中变更的顺序。        
 */
static void
AssertChangeLsnOrder(ReorderBufferTXN *txn)
{
#ifdef USE_ASSERT_CHECKING
	dlist_iter	iter;
	XLogRecPtr	prev_lsn = txn->first_lsn;

	dlist_foreach(iter, &txn->changes)
	{
		ReorderBufferChange *cur_change;

		cur_change = dlist_container(ReorderBufferChange, node, iter.cur);

		Assert(txn->first_lsn != InvalidXLogRecPtr);
		Assert(cur_change->lsn != InvalidXLogRecPtr);
		Assert(txn->first_lsn <= cur_change->lsn);

		if (txn->end_lsn != InvalidXLogRecPtr)
			Assert(cur_change->lsn <= txn->end_lsn);

		Assert(prev_lsn <= cur_change->lsn);

		prev_lsn = cur_change->lsn;
	}
#endif
}

/*
 * ReorderBufferGetOldestTXN
 *		返回重排序缓冲区最旧的事务   
 */
ReorderBufferTXN *
ReorderBufferGetOldestTXN(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->toplevel_by_lsn))
		return NULL;

	txn = dlist_head_element(ReorderBufferTXN, node, &rb->toplevel_by_lsn);

	Assert(!rbtxn_is_known_subxact(txn));
	Assert(txn->first_lsn != InvalidXLogRecPtr);
	return txn;
}

/*
 * ReorderBufferGetOldestXmin
 *		返回最旧的 Xmin               
 *
 * 返回从重排序缓冲区所保留事务使用的快照角度来看  
 * 可能仍在运行的 Xid，若无则返回 InvalidTransactionId          
 *                
 *
 * 由于快照按单调顺序分配，这等于 base_snapshot_lsn        
 * 最小的那个基础快照的 Xmin。       
 */
TransactionId
ReorderBufferGetOldestXmin(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	AssertTXNLsnOrder(rb);

	if (dlist_is_empty(&rb->txns_by_base_snapshot_lsn))
		return InvalidTransactionId;

	txn = dlist_head_element(ReorderBufferTXN, base_snapshot_node,
							 &rb->txns_by_base_snapshot_lsn);
	return txn->base_snapshot->xmin;
}

void
ReorderBufferSetRestartPoint(ReorderBuffer *rb, XLogRecPtr ptr)
{
	rb->current_restart_decoding_lsn = ptr;
}

/*
 * ReorderBufferAssignChild
 *
 * 记下我们知道 subxid 是 xid 的子事务，见于                  
 * 该 lsn。    
 */
void
ReorderBufferAssignChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXN *subtxn;
	bool		new_top;
	bool		new_sub;

	txn = ReorderBufferTXNByXid(rb, xid, true, &new_top, lsn, true);
	subtxn = ReorderBufferTXNByXid(rb, subxid, true, &new_sub, lsn, false);

	if (!new_sub)
	{
		if (rbtxn_is_known_subxact(subtxn))
		{
			/* 已关联，无事可做          */
			return;
		}
		else
		{
			/*
			 * 我们之前见过它，但最初加入了顶层事务列表  
			 * 中。现在知道它非顶层，就把它从那里移除。 
			 *                      
			 */
			dlist_delete(&subtxn->node);
		}
	}

	subtxn->txn_flags |= RBTXN_IS_SUBXACT;
	subtxn->toplevel_xid = xid;
	Assert(subtxn->nsubtxns == 0);

	/* 设置对顶层事务的引用             */
	subtxn->toptxn = txn;

	/* 加入子事务列表      */
	dlist_push_tail(&txn->subtxns, &subtxn->node);
	txn->nsubtxns++;

	/* 可能需要将子事务的快照转移给其顶层事务。  */
	ReorderBufferTransferSnapToParent(txn, subtxn);

	/* 验证 LSN 顺序不变量    */
	AssertTXNLsnOrder(rb);
}

/*
 * ReorderBufferTransferSnapToParent
 *		若需要，将基础快照从子事务转移到顶层事务  
 *
 * 若顶层事务没有基础快照，或者子事务的基础快          
 * 照的 LSN 早于顶层事务的基础快照 LSN，就需要执        
 * 行转移。若顶层事务无变更而子事务有变更，            
 * 或子事务首个变更的 LSN 早于顶层事务首个变更的      
 * LSN，且我们直到现在才得知二者的亲缘关系              
 * 时才会发生。     
 *
 * 无论是否发生转移，子事务的快照都会被清除，    
 * 因为在这两种情况下它都不再被需要。     
 *
 * 我们在得知二者的亲缘关系后立即执行该操作，          
 * 以避免向已知为子事务的事务排入额外快照，只     
 * 有顶层事务接收。  
 */
static void
ReorderBufferTransferSnapToParent(ReorderBufferTXN *txn,
								  ReorderBufferTXN *subtxn)
{
	Assert(subtxn->toplevel_xid == txn->xid);

	if (subtxn->base_snapshot != NULL)
	{
		if (txn->base_snapshot == NULL ||
			subtxn->base_snapshot_lsn < txn->base_snapshot_lsn)
		{
			/*
			 * 若顶层事务已有基础快照但比子事务新，则  
			 * 将其清除。                         
			 */
			if (txn->base_snapshot != NULL)
			{
				SnapBuildSnapDecRefcount(txn->base_snapshot);
				dlist_delete(&txn->base_snapshot_node);
			}

			/*
			 * 该快照现在属于顶层事务；将其转移，并     
			 * 通过把它移动到子事务所在的位置，来调整     
			 * 顶层事务在列表中的位置。     
			 */
			txn->base_snapshot = subtxn->base_snapshot;
			txn->base_snapshot_lsn = subtxn->base_snapshot_lsn;
			dlist_insert_before(&subtxn->base_snapshot_node,
								&txn->base_snapshot_node);

			/*
			 * 子事务不再拥有快照（因此不允许            
			 * 留在列表中。）   
			 */
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
			dlist_delete(&subtxn->base_snapshot_node);
		}
		else
		{
			/* 顶层基础快照可用，无需保留子事务的快照 */
			SnapBuildSnapDecRefcount(subtxn->base_snapshot);
			dlist_delete(&subtxn->base_snapshot_node);
			subtxn->base_snapshot = NULL;
			subtxn->base_snapshot_lsn = InvalidXLogRecPtr;
		}
	}
}

/*
 * 在提交时将子事务与其顶层事务关联起来。此后   
 * 此后无法再向其添加任何变更。             
 */
void
ReorderBufferCommitChild(ReorderBuffer *rb, TransactionId xid,
						 TransactionId subxid, XLogRecPtr commit_lsn,
						 XLogRecPtr end_lsn)
{
	ReorderBufferTXN *subtxn;

	subtxn = ReorderBufferTXNByXid(rb, subxid, false, NULL,
								   InvalidXLogRecPtr, false);

	/*
	 * 若该子事务不含任何变更，则无需做任何事       
	 */
	if (!subtxn)
		return;

	subtxn->final_lsn = commit_lsn;
	subtxn->end_lsn = end_lsn;

	/*
	 * 将此子事务指派为顶层事务子项（已完成则无操作   
	 * ）   
	 */
	ReorderBufferAssignChild(rb, xid, subxid, InvalidXLogRecPtr);
}


/*
 * 支持高效地遍历一个事务及其各子事务的        
 * 变更。                
 *
 * 具体做法是在各个事务/子事务之间做 k 路归并。为此，  
 * 我们将各事务当前的头部建模为一个二叉堆，这样       
 * 就能轻易知道接下来哪个（子）事务的变更 lsn 最小      
 * 。      
 *
 * 我们假定各事务内部的变更已按 LSN 排序。                   
 */

/*
 * 二叉堆比较函数。        
 */
static int
ReorderBufferIterCompare(Datum a, Datum b, void *arg)
{
	ReorderBufferIterTXNState *state = (ReorderBufferIterTXNState *) arg;
	XLogRecPtr	pos_a = state->entries[DatumGetInt32(a)].lsn;
	XLogRecPtr	pos_b = state->entries[DatumGetInt32(b)].lsn;

	if (pos_a < pos_b)
		return 1;
	else if (pos_a == pos_b)
		return 0;
	return -1;
}

/*
 * 分配并初始化迭代器，它按 lsn 顺序遍历一个事务   
 * 及其所有子事务。                
 *
 * 注意：迭代器状态通过 iter_state 参数返回，而不是      
 * 通过函数返回值。这是因为该状态会在调用者的             
 * PG_CATCH 块中被清理，因此我们要确保即使此函数抛        
 * 出异常，调用者也能取回该状态。            
 */
static void
ReorderBufferIterTXNInit(ReorderBuffer *rb, ReorderBufferTXN *txn,
						 ReorderBufferIterTXNState *volatile *iter_state)
{
	Size		nr_txns = 0;
	ReorderBufferIterTXNState *state;
	dlist_iter	cur_txn_i;
	int32		off;

	*iter_state = NULL;

	/* 检查顶层事务中变更的顺序。                */
	AssertChangeLsnOrder(txn);

	/*
	 * 计算堆的大小：每个包含变更的事务对应一个元素。 
	 * （除了重排序缓冲区中已有的事务之外，我们还    
	 * 会统计直接传给我们的那个事务。）  
	 */
	if (txn->nentries > 0)
		nr_txns++;

	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		/* 检查此子事务中变更的顺序。           */
		AssertChangeLsnOrder(cur_txn);

		if (cur_txn->nentries > 0)
			nr_txns++;
	}

	/* 分配迭代状态       */
	state = (ReorderBufferIterTXNState *)
		MemoryContextAllocZero(rb->context,
							   sizeof(ReorderBufferIterTXNState) +
							   sizeof(ReorderBufferIterTXNEntry) * nr_txns);

	state->nr_txns = nr_txns;
	dlist_init(&state->old_change);

	for (off = 0; off < state->nr_txns; off++)
	{
		state->entries[off].file.vfd = -1;
		state->entries[off].segno = 0;
	}

	/* 分配堆     */
	state->heap = binaryheap_allocate(state->nr_txns,
									  ReorderBufferIterCompare,
									  state);

	/* 现在状态成员均已初始化，可以安全地将其返回。  */
	*iter_state = state;

	/*
	 * 现在以无序的方式将各条目插入二叉堆。（我们     
	 * 会在最后运行一次堆组装步骤；这样效率更高）   
	 */

	off = 0;

	/* 若顶层事务包含变更则将其加入      */
	if (txn->nentries > 0)
	{
		ReorderBufferChange *cur_change;

		if (rbtxn_is_serialized(txn))
		{
			/* 溢写剩余变更          */
			ReorderBufferSerializeTXN(rb, txn);
			ReorderBufferRestoreChanges(rb, txn, &state->entries[off].file,
										&state->entries[off].segno);
		}

		cur_change = dlist_head_element(ReorderBufferChange, node,
										&txn->changes);

		state->entries[off].lsn = cur_change->lsn;
		state->entries[off].change = cur_change;
		state->entries[off].txn = txn;

		binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
	}

	/* 若子事务包含变更则将其加入     */
	dlist_foreach(cur_txn_i, &txn->subtxns)
	{
		ReorderBufferTXN *cur_txn;

		cur_txn = dlist_container(ReorderBufferTXN, node, cur_txn_i.cur);

		if (cur_txn->nentries > 0)
		{
			ReorderBufferChange *cur_change;

			if (rbtxn_is_serialized(cur_txn))
			{
				/* 溢写剩余变更          */
				ReorderBufferSerializeTXN(rb, cur_txn);
				ReorderBufferRestoreChanges(rb, cur_txn,
											&state->entries[off].file,
											&state->entries[off].segno);
			}
			cur_change = dlist_head_element(ReorderBufferChange, node,
											&cur_txn->changes);

			state->entries[off].lsn = cur_change->lsn;
			state->entries[off].change = cur_change;
			state->entries[off].txn = cur_txn;

			binaryheap_add_unordered(state->heap, Int32GetDatum(off++));
		}
	}

	/* 组装出有效的二叉堆  */
	binaryheap_build(state->heap);
}

/*
 * 遍历一个事务及其子事务时返回下一个             
 * 变更。       
 *
 * 若无更多变更则返回 NULL。        
 */
static ReorderBufferChange *
ReorderBufferIterTXNNext(ReorderBuffer *rb, ReorderBufferIterTXNState *state)
{
	ReorderBufferChange *change;
	ReorderBufferIterTXNEntry *entry;
	int32		off;

	/* 已没有任何内容 */
	if (state->heap->bh_size == 0)
		return NULL;

	off = DatumGetInt32(binaryheap_first(state->heap));
	entry = &state->entries[off];

	/* 释放在上一次 *Next 调用中可能"泄漏"的内存     */
	if (!dlist_is_empty(&state->old_change))
	{
		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferReturnChange(rb, change, true);
		Assert(dlist_is_empty(&state->old_change));
	}

	change = entry->change;

	/*
	 * 更新堆，记录 LSN 顺序上下一个相关变更属于      
	 * 哪个事务。             
	 */

	/* 内存中还有变更       */
	if (dlist_has_next(&entry->txn->changes, &entry->change->node))
	{
		dlist_node *next = dlist_next_node(&entry->txn->changes, &change->node);
		ReorderBufferChange *next_change =
			dlist_container(ReorderBufferChange, node, next);

		/* 事务保持不变 */
		state->entries[off].lsn = next_change->lsn;
		state->entries[off].change = next_change;

		binaryheap_replace_first(state->heap, Int32GetDatum(off));
		return change;
	}

	/* 尝试从磁盘加载变更   */
	if (entry->txn->nentries != entry->txn->nentries_mem)
	{
		/*
		 * 丑陋之处：恢复变更会复用 *Change 记录，因此需要 
		 * 将其从每事务列表删除，下次调用才释放          
		 */
		dlist_delete(&change->node);
		dlist_push_tail(&state->old_change, &change->node);

		/*
		 * 对于我们正为其释放当前变更集并恢复新变   
		 * 更集的那个事务，更新它所处理的总字节           
		 * 数。  
		 */
		rb->totalBytes += entry->txn->size;
		if (ReorderBufferRestoreChanges(rb, entry->txn, &entry->file,
										&state->entries[off].segno))
		{
			/* 已成功从磁盘恢复变更          */
			ReorderBufferChange *next_change =
				dlist_head_element(ReorderBufferChange, node,
								   &entry->txn->changes);

			elog(DEBUG2, "restored %u/%u changes from disk",
				 (uint32) entry->txn->nentries_mem,
				 (uint32) entry->txn->nentries);

			Assert(entry->txn->nentries_mem);
			/* 事务保持不变 */
			state->entries[off].lsn = next_change->lsn;
			state->entries[off].change = next_change;
			binaryheap_replace_first(state->heap, Int32GetDatum(off));

			return change;
		}
	}

	/* 已无变更，将其移除          */
	binaryheap_remove_first(state->heap);

	return change;
}

/*
 * 释放迭代器        
 */
static void
ReorderBufferIterTXNFinish(ReorderBuffer *rb,
						   ReorderBufferIterTXNState *state)
{
	int32		off;

	for (off = 0; off < state->nr_txns; off++)
	{
		if (state->entries[off].file.vfd != -1)
			FileClose(state->entries[off].file.vfd);
	}

	/* 释放在上次 *Next 调用中可能"泄漏"的内存    */
	if (!dlist_is_empty(&state->old_change))
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node,
								 dlist_pop_head_node(&state->old_change));
		ReorderBufferReturnChange(rb, change, true);
		Assert(dlist_is_empty(&state->old_change));
	}

	binaryheap_free(state->heap);
	pfree(state);
}

/*
 * 清理一个事务的内容，通常在该事务提交或中止     
 * 之后。            
 */
static void
ReorderBufferCleanupTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	bool		found;
	dlist_mutable_iter iter;

	/* 清理子事务及其变更             */
	dlist_foreach_modify(iter, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, iter.cur);

		/*
		 * 子事务总是与顶层 TXN 相关联，即便它最初          
		 * 发生在另一个子事务中，因此这里                    
		 * 递归不会超过一层。                
		 */
		Assert(rbtxn_is_known_subxact(subtxn));
		Assert(subtxn->nsubtxns == 0);

		ReorderBufferCleanupTXN(rb, subtxn);
	}

	/* 清理该事务中的变更*/
	dlist_foreach_modify(iter, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* 检查未混入来自不同事务的变更。               */
		Assert(change->txn == txn);

		ReorderBufferReturnChange(rb, change, true);
	}

	/*
	 * 清理我们为解码系统表快照访问而存储的 tuplecids。  
	 * 它们总是存储在顶层事务中。            
	 */
	dlist_foreach_modify(iter, &txn->tuplecids)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* 检查未混入来自不同事务的变更。               */
		Assert(change->txn == txn);
		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

		ReorderBufferReturnChange(rb, change, true);
	}

	/*
	 * 已设置则清理基础快照。 
	 */
	if (txn->base_snapshot != NULL)
	{
		SnapBuildSnapDecRefcount(txn->base_snapshot);
		dlist_delete(&txn->base_snapshot_node);
	}

	/*
	 * 清理最后一次流式运行的快照。     
	 */
	if (txn->snapshot_now != NULL)
	{
		Assert(rbtxn_is_streamed(txn));
		ReorderBufferFreeSnap(rb, txn->snapshot_now);
	}

	/*
	 * 将 TXN 从所在列表中移除。  
	 *
	 * 注意：若 txn 为子事务，我们会把它从父事务的已 
	 * 知子事务列表中删除该 TXN；这会让父事务的         
	 * nsubxacts 计数偏高，但我们并不在意。否则，我们会   
	 * 把该 TXN 从按 LSN 排序的顶层 TXN 列表中删除。同时    
	 * 将其移出系统表修改事务列表。     
	 */
	dlist_delete(&txn->node);
	if (rbtxn_has_catalog_changes(txn))
		dclist_delete_from(&rb->catchange_txns, &txn->catchange_node);

	/* 现在从缓冲区中移除引用*/
	hash_search(rb->by_txn, &txn->xid, HASH_REMOVE, &found);
	Assert(found);

	/* 移除溢写到磁盘的条目 */
	if (rbtxn_is_serialized(txn))
		ReorderBufferRestoreCleanup(rb, txn);

	/* 释放     */
	ReorderBufferReturnTXN(rb, txn);
}

/*
 * 丢弃一个事务（及其子事务）的变更，时机可以是    
 * 在流式发送后，或 PREPARE 时解码后。保留其余    
 * 信息——事务、tuplecid、失效与快照。   
 *
 * 另外，在 prepare 时解码完该事务后，我们还会移除        
 * tuplecid，因为回滚或提交已准备事务时只需执行失效        
 *
 * 'txn_prepared' 表示我们已经解码了 prepare 时的事务。      
 *      
 */
static void
ReorderBufferTruncateTXN(ReorderBuffer *rb, ReorderBufferTXN *txn, bool txn_prepared)
{
	dlist_mutable_iter iter;

	/* 清理子事务及其变更             */
	dlist_foreach_modify(iter, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, iter.cur);

		/*
		 * 子事务总是与顶层 TXN 相关联，即便它最初          
		 * 发生在另一个子事务中，因此这里                    
		 * 递归不会超过一层。                
		 */
		Assert(rbtxn_is_known_subxact(subtxn));
		Assert(subtxn->nsubtxns == 0);

		ReorderBufferTruncateTXN(rb, subtxn, txn_prepared);
	}

	/* 清理该事务中的变更*/
	dlist_foreach_modify(iter, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		/* 检查未混入来自不同事务的变更。               */
		Assert(change->txn == txn);

		/* 将该变更从其所在列表中移除     */
		dlist_delete(&change->node);

		ReorderBufferReturnChange(rb, change, true);
	}

	/*
	 * 将事务标记为已流式。   
	 *
	 * 顶层事务总是会被标记为已流式，即使它不包含    
	 * 任何变更（也就是说，当所有变更都在子事务      
	 * 中时）。     
	 *
	 * 对于子事务，只有当其中有变更时才标记为        
	 * 已流式。    
	 *
	 * 我们这样做是因为中止的存在 —— 我们不想向下游  
	 * 发送其不知道的 XID 的中止。下游当然总是知道     
	 * 顶层事务（所有消息中我们都会发送 XID），但从不    
	 * 流式空子事务的 XID。  
	 */
	if ((!txn_prepared) && (rbtxn_is_toptxn(txn) || (txn->nentries_mem != 0)))
		txn->txn_flags |= RBTXN_IS_STREAMED;

	if (txn_prepared)
	{
		/*
		 * 若为已准备事务，清理我们为解码系统表快照  
		 * 访问而存储的 tuplecid，它们总存于顶层事务中   
		 * 。                  
		 */
		dlist_foreach_modify(iter, &txn->tuplecids)
		{
			ReorderBufferChange *change;

			change = dlist_container(ReorderBufferChange, node, iter.cur);

			/* 检查未混入来自不同事务的变更。               */
			Assert(change->txn == txn);
			Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

			/* 将该变更从其所在列表中移除。  */
			dlist_delete(&change->node);

			ReorderBufferReturnChange(rb, change, true);
		}
	}

	/*
	 * 销毁 (relfilelocator, ctid) 哈希表以免泄漏内存。           
	 * 我们也可以保留哈希表并用新的 ctid 值去更新它，   
	 * 但目前这样更简单且已足够。                
	 */
	if (txn->tuplecid_hash != NULL)
	{
		hash_destroy(txn->tuplecid_hash);
		txn->tuplecid_hash = NULL;
	}

	/* 若此事务已序列化，则清理磁盘空间。  */
	if (rbtxn_is_serialized(txn))
	{
		ReorderBufferRestoreCleanup(rb, txn);
		txn->txn_flags &= ~RBTXN_IS_SERIALIZED;

		/*
		 * 我们设置此标志以表明该事务是否曾被序列化。    
		 * 我们需要它来准确更新统计信息，否则同一事务  
		 * 可能会被多次计为已序列化。                 
		 */
		txn->txn_flags |= RBTXN_IS_SERIALIZED_CLEAR;
	}

	/* 同时重置事务中的条目数                   */
	txn->nentries_mem = 0;
	txn->nentries = 0;
}

/*
 * 构建 (relfilelocator, ctid) -> (cmin, cmax) 映射的哈希表，供       
 * HeapTupleSatisfiesHistoricMVCC.
 */
static void
ReorderBufferBuildTupleCidHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	iter;
	HASHCTL		hash_ctl;

	if (!rbtxn_has_catalog_changes(txn) || dlist_is_empty(&txn->tuplecids))
		return;

	hash_ctl.keysize = sizeof(ReorderBufferTupleCidKey);
	hash_ctl.entrysize = sizeof(ReorderBufferTupleCidEnt);
	hash_ctl.hcxt = rb->context;

	/*
	 * 从一开始就按将要存储的 tuplecid 的确切数量创建    
	 * 哈希   
	 */
	txn->tuplecid_hash =
		hash_create("ReorderBufferTupleCid", txn->ntuplecids, &hash_ctl,
					HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	dlist_foreach(iter, &txn->tuplecids)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		bool		found;
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, iter.cur);

		Assert(change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID);

		/* 注意填充问题       */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		key.rlocator = change->data.tuplecid.locator;

		ItemPointerCopy(&change->data.tuplecid.tid,
						&key.tid);

		ent = (ReorderBufferTupleCidEnt *)
			hash_search(txn->tuplecid_hash, &key, HASH_ENTER, &found);
		if (!found)
		{
			ent->cmin = change->data.tuplecid.cmin;
			ent->cmax = change->data.tuplecid.cmax;
			ent->combocid = change->data.tuplecid.combocid;
		}
		else
		{
			/*
			 * 也许我们在此事务中已见过该元组，但若是      
			 * 这样，其 cmin 必相同。   
			 */
			Assert(ent->cmin == change->data.tuplecid.cmin);

			/*
			 * cmax 起初可能无效，但一旦设置就只能增长，  
			 * 绝不会再变为无效。    
			 */
			Assert((ent->cmax == InvalidCommandId) ||
				   ((change->data.tuplecid.cmax != InvalidCommandId) &&
					(change->data.tuplecid.cmax > ent->cmax)));
			ent->cmax = change->data.tuplecid.cmax;
		}
	}
}

/*
 * 复制提供的快照，以便我们能私自修改它。这是          
 * 必需的，这样修改系统表的事务能查看中间状态       
 * 。    
 */
static Snapshot
ReorderBufferCopySnap(ReorderBuffer *rb, Snapshot orig_snap,
					  ReorderBufferTXN *txn, CommandId cid)
{
	Snapshot	snap;
	dlist_iter	iter;
	int			i = 0;
	Size		size;

	size = sizeof(SnapshotData) +
		sizeof(TransactionId) * orig_snap->xcnt +
		sizeof(TransactionId) * (txn->nsubtxns + 1);

	snap = MemoryContextAllocZero(rb->context, size);
	memcpy(snap, orig_snap, sizeof(SnapshotData));

	snap->copied = true;
	snap->active_count = 1;		/* 标记为活跃以免被释放    */
	snap->regd_count = 0;
	snap->xip = (TransactionId *) (snap + 1);

	memcpy(snap->xip, orig_snap->xip, sizeof(TransactionId) * snap->xcnt);

	/*
	 * snap->subxip 包含属于我们事务、需要通过 cmin/cmax         
	 * 检查的 txid，所以也把顶层事务一并存入其中  
	 * 。                          
	 */
	snap->subxip = snap->xip + snap->xcnt;
	snap->subxip[i++] = txn->xid;

	/*
	 * 子事务中止时 subxcnt 并不会减少，因此要手动计数。 
	 * 由于它是一个上界，所以将其用于上面的分配        
	 * 。   
	 */
	snap->subxcnt = 1;

	dlist_foreach(iter, &txn->subtxns)
	{
		ReorderBufferTXN *sub_txn;

		sub_txn = dlist_container(ReorderBufferTXN, node, iter.cur);
		snap->subxip[i++] = sub_txn->xid;
		snap->subxcnt++;
	}

	/* 排序以便之后 bsearch()   */
	qsort(snap->subxip, snap->subxcnt, sizeof(TransactionId), xidComparator);

	/* 存储指定的当前 CommandId       */
	snap->curcid = cid;

	return snap;
}

/*
 * 释放之前 ReorderBufferCopySnap 过的快照    
 */
static void
ReorderBufferFreeSnap(ReorderBuffer *rb, Snapshot snap)
{
	if (snap->copied)
		pfree(snap);
	else
		SnapBuildSnapDecRefcount(snap);
}

/*
 * 如果事务被（部分）流式发送过，就需要以"流式"方     
 * 式对它 prepare 或提交。即先流式发送事务的剩余部         
 * 分，然后根据情况调用 stream_prepare 或 stream_commit            
 * 消息   
 */
static void
ReorderBufferStreamCommit(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	/* 只应为之前已流式发送过的事务调用此函数     */
	Assert(rbtxn_is_streamed(txn));

	ReorderBufferStreamTXN(rb, txn);

	if (rbtxn_prepared(txn))
	{
		/*
		 * 注意：即使检测到并发中止也会发送 stream   
		 *  prepare。更多信息参见 DecodePrepare。   
		 */
		rb->stream_prepare(rb, txn, txn->final_lsn);

		/*
		 * 这是 PREPARED 事务，属于两阶段提交。完整清理  
		 * 将在 COMMIT PREPARED 时进行，现在只需通过移除变  
		 * 更和 tuplecid 来截断事务。                  
		 */
		ReorderBufferTruncateTXN(rb, txn, true);
		/* 重置 CheckXidAlive    */
		CheckXidAlive = InvalidTransactionId;
	}
	else
	{
		rb->stream_commit(rb, txn, txn->final_lsn);
		ReorderBufferCleanupTXN(rb, txn);
	}
}

/*
 * 设置 xid 以检测并发中止。 
 *
 * 在流式发送进行中的事务或解码已准备的事务     
 * 时，该（子）事务有可能被并发中止。在这种情况下 
 * 如果该（子）事务有系统表更新，我们就可能用错误  
 * 的系统表版本解码元组。例如假设有一个系统表元组  
 *  (xmin: 500, xmax: 0)。现在事务 501 更新了该系统表元           
 * 组，之后我们会得到两个元组 (xmin: 500, xmax: 501)           
 *  和 (xmin: 501, xmax: 0)。此时，如果 501 中止，而另            
 * 一个事务（比如 502）更新了同一系统表元组，那么第   
 * 一个元组会变为 (xmin: 500, xmax: 502)。问题在于：          
 * 当我们尝试解码 501 在系统表更新之后插入/更新的     
 * 元组时，我们会看到 (xmin: 500, xmax: 502) 这一系统表      
 * 元组是可见的，因为它会认为该元组已被 xid 502             
 * 删除，而 502 对我们的快照不可见。而当我们尝试     
 * 用该系统表元组进行解码时，就可能导致错误的结       
 * 果或崩溃。因此，必须检测并发中止，才能允许流   
 * 式发送进行中的事务或解码已准备的事务。     
 *
 * 为了检测并发中止，我们把 CheckXidAlive 设为当前       
 * （子）事务的 xid，即此变更所属的事务。另外，在  
 * 系统表扫描期间我们可以检查该 xid 的状态，如果它已     
 * 中止，我们就会报告特定错误，以便在此类错误上       
 * 停止流式发送当前事务并丢弃已流式发送的变更。       
 * 我们可能已经流式发送了已中止（子）事务的部分        
 * 变更，但这无妨，因为解码中止时我们会流式发送        
 * 中止消息截断订阅端的变更。类似地，对已准备   
 * 事务，如果检测到并发中止，我们就停止解码，并     
 * 在遇到 rollback_prepared 时回滚这些变更。参见     
 * DecodePrepare 
 */
static inline void
SetupCheckXidLive(TransactionId xid)
{
	/*
	 * 如果输入的事务 ID 已被设置为 CheckXidAlive，则      
	 * 无事可做  
	 */
	if (TransactionIdEquals(CheckXidAlive, xid))
		return;

	/*
	 * 若尚未提交，则设置 CheckXidAlive。我们不检查 xid      
	 * 是否已中止。那会在访问系统表时进行。  
	 */
	if (!TransactionIdDidCommit(xid))
		CheckXidAlive = xid;
	else
		CheckXidAlive = InvalidTransactionId;
}

/*
 * ReorderBufferProcessTXN 用于应用变更的辅助函数。    
 */
static inline void
ReorderBufferApplyChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
						 Relation relation, ReorderBufferChange *change,
						 bool streaming)
{
	if (streaming)
		rb->stream_change(rb, txn, relation, change);
	else
		rb->apply_change(rb, txn, relation, change);
}

/*
 * ReorderBufferProcessTXN 用于应用 truncate 的辅助函数。      
 */
static inline void
ReorderBufferApplyTruncate(ReorderBuffer *rb, ReorderBufferTXN *txn,
						   int nrelations, Relation *relations,
						   ReorderBufferChange *change, bool streaming)
{
	if (streaming)
		rb->stream_truncate(rb, txn, nrelations, relations, change);
	else
		rb->apply_truncate(rb, txn, nrelations, relations, change);
}

/*
 * ReorderBufferProcessTXN 用于应用消息的辅助函数。         
 */
static inline void
ReorderBufferApplyMessage(ReorderBuffer *rb, ReorderBufferTXN *txn,
						  ReorderBufferChange *change, bool streaming)
{
	if (streaming)
		rb->stream_message(rb, txn, change->lsn, true,
						   change->data.msg.prefix,
						   change->data.msg.message_size,
						   change->data.msg.message);
	else
		rb->message(rb, txn, change->lsn, true,
					change->data.msg.prefix,
					change->data.msg.message_size,
					change->data.msg.message);
}

/*
 * 该函数在当前流结束时存储命令 ID 与快照，使我       
 * 们可以在发送下一个流时复用它们。                   
 */
static inline void
ReorderBufferSaveTXNSnapshot(ReorderBuffer *rb, ReorderBufferTXN *txn,
							 Snapshot snapshot_now, CommandId command_id)
{
	txn->command_id = command_id;

	/* 已复制过则避免再次复制。  */
	if (snapshot_now->copied)
		txn->snapshot_now = snapshot_now;
	else
		txn->snapshot_now = ReorderBufferCopySnap(rb, snapshot_now,
												  txn, command_id);
}

/*
 * ReorderBufferProcessTXN 的辅助函数，用于处理              
 * 流式事务的并发中止。它重置 TXN，使之可用于继     
 * 续流式发送正在处理的事务的剩余数据。当子事务      
 * 被中止而我们仍想继续处理主事务或其他子事务        
 * 的数据时发生。                                      
 */
static void
ReorderBufferResetTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
					  Snapshot snapshot_now,
					  CommandId command_id,
					  XLogRecPtr last_lsn,
					  ReorderBufferChange *specinsert)
{
	/* 丢弃我们刚刚流式发送的变更   */
	ReorderBufferTruncateTXN(rb, txn, rbtxn_prepared(txn));

	/* 释放为 TOAST 重建分配的所有资源           */
	ReorderBufferToastReset(rb, txn);

	/* 若 spec 插入变更不为 NULL 则将其返回*/
	if (specinsert != NULL)
	{
		ReorderBufferReturnChange(rb, specinsert, true);
		specinsert = NULL;
	}

	/*
	 * 对于流式情况，停止流并记住命令 ID 与本次             
	 * 流式运行使用的快照。 
	 */
	if (rbtxn_is_streamed(txn))
	{
		rb->stream_stop(rb, txn, last_lsn);
		ReorderBufferSaveTXNSnapshot(rb, txn, snapshot_now, command_id);
	}
}

/*
 * ReorderBufferReplay 与 ReorderBufferStreamTXN 的辅助函数。  
 *
 * 将一个事务（及其子事务）的数据发送给     
 * 输出插件。我们遍历顶层事务与子事务（以 k 路          
 * 归并）按 lsn 顺序重放变更。     
 *
 * 若 streaming 为真，则数据以 stream API 发送。     
 *
 * 注意：某些参数上的 "volatile" 标记用于避免函数      
 * 内的 PG_TRY 问题。    
 */
static void
ReorderBufferProcessTXN(ReorderBuffer *rb, ReorderBufferTXN *txn,
						XLogRecPtr commit_lsn,
						volatile Snapshot snapshot_now,
						volatile CommandId command_id,
						bool streaming)
{
	bool		using_subtxn;
	MemoryContext ccxt = CurrentMemoryContext;
	ReorderBufferIterTXNState *volatile iterstate = NULL;
	volatile XLogRecPtr prev_lsn = InvalidXLogRecPtr;
	ReorderBufferChange *volatile specinsert = NULL;
	volatile bool stream_started = false;
	ReorderBufferTXN *volatile curtxn = NULL;

	/* 构建数据以便查找系统表元组的 CommandId             */
	ReorderBufferBuildTupleCidHash(rb, txn);

	/* 设置初始快照         */
	SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);

	/*
	 * 解码需要访问 syscache 等，其内又使用重量级  
	 * 锁等资源。因此我们需要维护足够的状态来跟踪        
	 * 它们。最简单的办法就是直接在内部使用一个事     
	 * 务。这也让我们能方便地通过检查 XID 分配来           
	 * 强制确保无任何内容写入数据库。   
	 *
	 * 当我们经 SQL SRF 被调用时，事务已经启动，      
	 * 因此在那里启动一个显式子事务。      
	 */
	using_subtxn = IsTransactionOrTransactionBlock();

	PG_TRY();
	{
		ReorderBufferChange *change;
		int			changes_count = 0;	/* used to accumulate the number of
										 * changes */

		if (using_subtxn)
			BeginInternalSubTransaction(streaming ? "stream" : "replay");
		else
			StartTransactionCommand();

		/*
		 * 只需为非流式事务发送 begin/begin-prepare。    
		 *              
		 */
		if (!streaming)
		{
			if (rbtxn_prepared(txn))
				rb->begin_prepare(rb, txn);
			else
				rb->begin(rb, txn);
		}

		ReorderBufferIterTXNInit(rb, txn, &iterstate);
		while ((change = ReorderBufferIterTXNNext(rb, iterstate)) != NULL)
		{
			Relation	relation = NULL;
			Oid			reloid;

			CHECK_FOR_INTERRUPTS();

			/*
			 * 在处理首个变更前，无法调用流开始回调。  
			 *        
			 */
			if (prev_lsn == InvalidXLogRecPtr)
			{
				if (streaming)
				{
					txn->origin_id = change->origin_id;
					rb->stream_start(rb, txn, change->lsn);
					stream_started = true;
				}
			}

			/*
			 * 对合并自多个子事务的变更，强制实施      
			 * 正确的顺序。MULTI_INSERT xlog 记录可能使变   
			 * 更具有相同的 LSN。 
			 */
			Assert(prev_lsn == InvalidXLogRecPtr || prev_lsn <= change->lsn);

			prev_lsn = change->lsn;

			/*
			 * 设置当前 XID 以检测并发中止。对于在      
			 * COMMIT 记录被处理之前解码变更的情形，这是  
			 * 必需的。               
			 */
			if (streaming || rbtxn_prepared(change->txn))
			{
				curtxn = change->txn;
				SetupCheckXidLive(curtxn->xid);
			}

			switch (change->action)
			{
				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:

					/*
					 * 推测插入的确认已到达。只需将其当作   
					 * 普通记录使用。它会在 INSERT 处理结尾    
					 * 被清理掉。      
					 */
					if (specinsert == NULL)
						elog(ERROR, "invalid ordering of speculative insertion changes");
					Assert(specinsert->data.tp.oldtuple == NULL);
					change = specinsert;
					change->action = REORDER_BUFFER_CHANGE_INSERT;

					/* intentionally fall through */
				case REORDER_BUFFER_CHANGE_INSERT:
				case REORDER_BUFFER_CHANGE_UPDATE:
				case REORDER_BUFFER_CHANGE_DELETE:
					Assert(snapshot_now);

					reloid = RelidByRelfilenumber(change->data.tp.rlocator.spcOid,
												  change->data.tp.rlocator.relNumber);

					/*
					 * 已映射但无数据的元组，产生于      
					 * 系统表正被重写期间。我们可能无法查找 
					 * relfilenumber，因为 relmapper 没有历史     
					 * 视图，这与解码时的正常系统表不同。 
					 * 因此反复重写可能导致查找失败。这没   
					 * 关系，反正我们不解码系统表变更。这类 
					 * 元组本会在下方被跳过，但若不将      
					 * relfilenumber 映射到 oid，则无法判定     
					 * 该表是否应当被逻辑记录。                
					 *                                      
					 */
					if (reloid == InvalidOid &&
						change->data.tp.newtuple == NULL &&
						change->data.tp.oldtuple == NULL)
						goto change_done;
					else if (reloid == InvalidOid)
						elog(ERROR, "could not map filenumber \"%s\" to relation OID",
							 relpathperm(change->data.tp.rlocator,
										 MAIN_FORKNUM));

					relation = RelationIdGetRelation(reloid);

					if (!RelationIsValid(relation))
						elog(ERROR, "could not open relation with OID %u (for filenumber \"%s\")",
							 reloid,
							 relpathperm(change->data.tp.rlocator,
										 MAIN_FORKNUM));

					if (!RelationIsLogicallyLogged(relation))
						goto change_done;

					/*
					 * 忽略 DDL 期间创建的临时堆，除非插件  
					 * 要求提供它们。     
					 */
					if (relation->rd_rel->relrewrite && !rb->output_rewrites)
						goto change_done;

					/*
					 * 目前完全忽略序列变更。多数时候它们  
					 * 并不以我们理解的记录记录变更  
					 * 因此处理我们所能处理的少数情况并无   
					 * 意义。   
					 */
					if (relation->rd_rel->relkind == RELKIND_SEQUENCE)
						goto change_done;

					/* 用户触发的变更 */
					if (!IsToastRelation(relation))
					{
						ReorderBufferToastReplace(rb, txn, relation, change);
						ReorderBufferApplyChange(rb, txn, relation, change,
												 streaming);

						/*
						 * 只在确定其不再需要时才清除重新组 
						 * 装的 TOAST 块。元组的创建者会告诉  
						 * 我们。      
						 */
						if (change->data.tp.clear_toast_afterwards)
							ReorderBufferToastReset(rb, txn);
					}
					/* 我们不关心 TOAST 删除操作      */
					else if (change->action == REORDER_BUFFER_CHANGE_INSERT)
					{
						/*
						 * 需在内存中重组完整的已 TOAST     
						 * Datum，确保这些块在我们完成前不会   
						 * 被重用，并将其移出该事务      
						 * 变更列表，否则恢复磁盘上的溢  
						 * 写数据时，它会被释放或重用。    
						 *      
						 */
						Assert(change->data.tp.newtuple != NULL);

						dlist_delete(&change->node);
						ReorderBufferToastAppendChunk(rb, txn, relation,
													  change);
					}

			change_done:

					/*
					 * 如果推测插入已获确认，该记录就不  
					 * 再需要了。      
					 */
					if (specinsert != NULL)
					{
						ReorderBufferReturnChange(rb, specinsert, true);
						specinsert = NULL;
					}

					if (RelationIsValid(relation))
					{
						RelationClose(relation);
						relation = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:

					/*
					 * 推测插入的处理方式是将其延迟到确认  
					 * 记录到达时。为此我们直接把该记录从   
					 * 链上摘除，这样在从磁盘恢复已溢写的   
					 * 数据期间不会被释放或重用。               
					 *                        
					 *
					 * 在并发系统表变更下这样做是安全的，   
					 * 因为在推测插入与确认之间相关的关系   
					 * 无法改动，由于 CheckTableNotInUse()   
					 * 与锁机制。                  
					 */

					/* 清除一个未决（因而已失败）的推测  */
					if (specinsert != NULL)
					{
						ReorderBufferReturnChange(rb, specinsert, true);
						specinsert = NULL;
					}

					/* 并记住这个待处理的插入  */
					dlist_delete(&change->node);
					specinsert = change;
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:

					/*
					 * 推测插入的中止已到达。所以要清理       
					 * specinsert 元组和哈希。   
					 *
					 * 注意，每个 toast 条目都会收到一条 spec  
					 * 中止变更，但只需在主表首次收到它时执 
					 * 行清理。                      
					 */
					if (specinsert != NULL)
					{
						/*
						 * 在处理一个全新元组前必须清理      
						 * TOAST 哈希，以免与前一元组的 TOAST    
						 * 块混淆。                  
						 */
						Assert(change->data.tp.clear_toast_afterwards);
						ReorderBufferToastReset(rb, txn);

						/* 这条记录不再需要了。     */
						ReorderBufferReturnChange(rb, specinsert, true);
						specinsert = NULL;
					}
					break;

				case REORDER_BUFFER_CHANGE_TRUNCATE:
					{
						int			i;
						int			nrelids = change->data.truncate.nrelids;
						int			nrelations = 0;
						Relation   *relations;

						relations = palloc0(nrelids * sizeof(Relation));
						for (i = 0; i < nrelids; i++)
						{
							Oid			relid = change->data.truncate.relids[i];
							Relation	rel;

							rel = RelationIdGetRelation(relid);

							if (!RelationIsValid(rel))
								elog(ERROR, "could not open relation with OID %u", relid);

							if (!RelationIsLogicallyLogged(rel))
								continue;

							relations[nrelations++] = rel;
						}

						/* 执行截断。     */
						ReorderBufferApplyTruncate(rb, txn, nrelations,
												   relations, change,
												   streaming);

						for (i = 0; i < nrelations; i++)
							RelationClose(relations[i]);

						break;
					}

				case REORDER_BUFFER_CHANGE_MESSAGE:
					ReorderBufferApplyMessage(rb, txn, change, streaming);
					break;

				case REORDER_BUFFER_CHANGE_INVALIDATION:
					/* 在本地执行这些失效消息         */
					ReorderBufferExecuteInvalidations(change->data.inval.ninvalidations,
													  change->data.inval.invalidations);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
					/* 丢掉旧的       */
					TeardownHistoricSnapshot(false);

					if (snapshot_now->copied)
					{
						ReorderBufferFreeSnap(rb, snapshot_now);
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}

					/*
					 * 从磁盘恢复，需小心避免二次释放，    
					 * 为此可引入引用计数来解决，但就目前   
					 * 来看它已足够罕见，无须在意。   
					 */
					else if (change->data.snapshot->copied)
					{
						snapshot_now =
							ReorderBufferCopySnap(rb, change->data.snapshot,
												  txn, command_id);
					}
					else
					{
						snapshot_now = change->data.snapshot;
					}

					/* 然后继续用新的那个   */
					SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);
					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
					Assert(change->data.command_id != InvalidCommandId);

					if (command_id < change->data.command_id)
					{
						command_id = change->data.command_id;

						if (!snapshot_now->copied)
						{
							/* 我们不再使用全局的那个了*/
							snapshot_now = ReorderBufferCopySnap(rb, snapshot_now,
																 txn, command_id);
						}

						snapshot_now->curcid = command_id;

						TeardownHistoricSnapshot(false);
						SetupHistoricSnapshot(snapshot_now, txn->tuplecid_hash);
					}

					break;

				case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
					elog(ERROR, "tuplecid value in changequeue");
					break;
			}

			/*
			 * 数据可能长时间未发送到下游，原因要么是   
			 * 输出插件将其过滤，要么是某条 DDL 产生了       
			 * 大量插件未处理的数据。在这种情况下，下游  
			 * 可能超时。为避免这一点，会在必要时尝试发 
			 * 送保活消息。每条变更后都尝试发送会有一 
			 * 定开销，但测试表明，如果每处理约 100 条变   
			 * 更才发送一次，并不会带来明显的开销。         
			 *                                   
			 */
#define CHANGES_THRESHOLD 100

			if (++changes_count >= CHANGES_THRESHOLD)
			{
				rb->update_progress_txn(rb, txn, prev_lsn);
				changes_count = 0;
			}
		}

		/* 推测插入记录此刻必定已被释放        */
		Assert(!specinsert);

		/* 清理迭代器       */
		ReorderBufferIterTXNFinish(rb, iterstate);
		iterstate = NULL;

		/*
		 * 更新事务总数，以及此事务及其子事务已         
		 * 处理的总字节数。注意不要把已流式的事务   
		 * 重复计入多次。               
		 *
		 * 统计计算必须在 ReorderBufferIterTXNFinish         
		 * 之后进行，因为它释放的已序列化变更已在      
		 * ReorderBufferIterTXNNext 中计入过。                    
		 */
		if (!rbtxn_is_streamed(txn))
			rb->totalTxns++;

		rb->totalBytes += txn->total_size;

		/*
		 * 当前变更已处理完毕，根据流式模式为这          
		 * 组变更发送最后一条消息。  
		 */
		if (streaming)
		{
			if (stream_started)
			{
				rb->stream_stop(rb, txn, prev_lsn);
				stream_started = false;
			}
		}
		else
		{
			/*
			 * 两阶段事务调用 PREPARE，普通事务则调用          
			 * COMMIT。     
			 */
			if (rbtxn_prepared(txn))
				rb->prepare(rb, txn, commit_lsn);
			else
				rb->commit(rb, txn, commit_lsn);
		}

		/* 这只是针对输出插件不良行为的一次健全性检查 */
		if (GetCurrentTransactionIdIfAny() != InvalidTransactionId)
			elog(ERROR, "output plugin used XID %u",
				 GetCurrentTransactionId());

		/*
		 * 流式模式下为下一组变更记住命令 ID 和快照。      
		 *                
		 */
		if (streaming)
			ReorderBufferSaveTXNSnapshot(rb, txn, snapshot_now, command_id);
		else if (snapshot_now->copied)
			ReorderBufferFreeSnap(rb, snapshot_now);

		/* 清理  */
		TeardownHistoricSnapshot(false);

		/*
		 * 将当前（子）事务作为一个整体中止有着正确   
		 * 的语义。我们希望在此获取的所有锁都被释放     
		 * 而非重新分配给父事务，也不希望数据库访问   
		 * 产生持久影响。   
		 */
		AbortCurrentTransaction();

		/* 确保没有缓存污染             */
		if (rbtxn_distr_inval_overflowed(txn))
		{
			Assert(txn->ninvalidations_distributed == 0);
			InvalidateSystemCaches();
		}
		else
		{
			ReorderBufferExecuteInvalidations(txn->ninvalidations, txn->invalidations);
			ReorderBufferExecuteInvalidations(txn->ninvalidations_distributed,
											  txn->invalidations_distributed);
		}

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		/*
		 * 我们到此的原因有四种：1. 解码进行中的    
		 * 事务。2. 解码已预备事务。3. 解码（部分）流 
		 * 式发送过的已预备事务。4. 解码已提交事务。       
		 *     
		 *
		 * 对于 1，我们允许截断事务数据，即移除已      
		 * 流式发送的变更，但仍保留失效消息、快照          
		 * 和 tuplecids。对于 2 和 3，我们指示    
		 * ReorderBufferTruncateTXN 对事务数据做更精细           
		 * 的截断，因为除提交之外整个事务已被解码。      
		 * 对于 4，整个事务既已解码，可完全清理 TXN       
		 * 重排序缓冲区。  
		 */
		if (streaming || rbtxn_prepared(txn))
		{
			ReorderBufferTruncateTXN(rb, txn, rbtxn_prepared(txn));
			/* 重置 CheckXidAlive    */
			CheckXidAlive = InvalidTransactionId;
		}
		else
			ReorderBufferCleanupTXN(rb, txn);
	}
	PG_CATCH();
	{
		MemoryContext ecxt = MemoryContextSwitchTo(ccxt);
		ErrorData  *errdata = CopyErrorData();

		/* TODO: 将 PG_TRY 与 PG_CATCH 块中的清理封装起来     */
		if (iterstate)
			ReorderBufferIterTXNFinish(rb, iterstate);

		TeardownHistoricSnapshot(true);

		/*
		 * 强制在有效事务之外执行缓存失效，以阻止对     
		 * 系统表的访问，因为刚刚捕获了错误。  
		 */
		AbortCurrentTransaction();

		/* 确保没有缓存污染             */
		if (rbtxn_distr_inval_overflowed(txn))
		{
			Assert(txn->ninvalidations_distributed == 0);
			InvalidateSystemCaches();
		}
		else
		{
			ReorderBufferExecuteInvalidations(txn->ninvalidations, txn->invalidations);
			ReorderBufferExecuteInvalidations(txn->ninvalidations_distributed,
											  txn->invalidations_distributed);
		}

		if (using_subtxn)
			RollbackAndReleaseCurrentSubTransaction();

		/*
		 * 错误代码 ERRCODE_TRANSACTION_ROLLBACK 表示我们正在流   
		 * 式发送或预备的（子）事务被并发中止。我      
		 * 们需要完成清理并就此错误优雅地返回，参见   
		 * SetupCheckXidLive 
		 *
		 * 解码期间调用的回调之一可能抛出此错误代    
		 * 码，因此我们需要确保只有当我们在流式模式       
		 * 下发送数据且流式发送尚未结束，或者当我          
		 * 们在两阶段提交期间通过 PREPARE 发送数据时        
		 *                           
		 */
		if (errdata->sqlerrcode == ERRCODE_TRANSACTION_ROLLBACK &&
			(stream_started || rbtxn_prepared(txn)))
		{
			/* 对于流式或已预备事务，curtxn 必须已设置   */
			Assert(curtxn);

			/* 清理临时错误状态。        */
			FlushErrorState();
			FreeErrorData(errdata);
			errdata = NULL;
			curtxn->concurrent_abort = true;

			/* 重置 TXN，使其允许流式发送剩余数据。          */
			ReorderBufferResetTXN(rb, txn, snapshot_now,
								  command_id, prev_lsn,
								  specinsert);
		}
		else
		{
			ReorderBufferCleanupTXN(rb, txn);
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();
}

/*
 * 执行一个事务及其未中止子事务的重放。                  
 *
 * 子事务此前必须已经由                    
 * ReorderBufferCommitChild() 处理过，即便之前已通过           
 * ReorderBufferAssignChild 分给顶层。  
 *
 * 一旦读取到预备或顶层提交记录，对流式与                  
 * 非流式事务都会调用此接口。       
 */
static void
ReorderBufferReplay(ReorderBufferTXN *txn,
					ReorderBuffer *rb, TransactionId xid,
					XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
					TimestampTz commit_time,
					RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	Snapshot	snapshot_now;
	CommandId	command_id = FirstCommandId;

	txn->final_lsn = commit_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.commit_time = commit_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	/*
	 * 如果该事务曾经（部分）流式发送过，我们就需要    
	 * 以"流式"的方式提交它。也就是说，我们先流式    
	 * 发完剩余部分再调用 stream_commit。       
	 *
	 * 在所有内容（origin ID、LSN 等）都存入该事务    
	 * 之后再调用，以免直接传递这些信息。    
	 */
	if (rbtxn_is_streamed(txn))
	{
		ReorderBufferStreamCommit(rb, txn);
		return;
	}

	/*
	 * 如果该事务没有快照，说明它没有对数据库做过       
	 * 任何变更，故无内容可解码。注意     
	 * ReorderBufferCommitChild 将已把子事务中的快照          
	 * 都转移过来（若有）。    
	 */
	if (txn->base_snapshot == NULL)
	{
		Assert(txn->ninvalidations == 0);

		/*
		 * 在提交之前移除此事务，可能导致计算出错误     
		 * 的 restart_lsn。参见 SnapBuildProcessRunningXacts。      
		 */
		if (!rbtxn_prepared(txn))
			ReorderBufferCleanupTXN(rb, txn);
		return;
	}

	snapshot_now = txn->base_snapshot;

	/* 处理变更并将其发送给输出插件。  */
	ReorderBufferProcessTXN(rb, txn, commit_lsn, snapshot_now,
							command_id, false);
}

/*
 * 提交事务。      
 *
 * 参见 ReorderBufferReplay()。        
 */
void
ReorderBufferCommit(ReorderBuffer *rb, TransactionId xid,
					XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
					TimestampTz commit_time,
					RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* 未知事务，没有可重放的内容*/
	if (txn == NULL)
		return;

	ReorderBufferReplay(txn, rb, xid, commit_lsn, end_lsn, commit_time,
						origin_id, origin_lsn);
}

/*
 * 记录一个事务的预备信息。             
 */
bool
ReorderBufferRememberPrepareInfo(ReorderBuffer *rb, TransactionId xid,
								 XLogRecPtr prepare_lsn, XLogRecPtr end_lsn,
								 TimestampTz prepare_time,
								 RepOriginId origin_id, XLogRecPtr origin_lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr, false);

	/* 未知事务，无事可做        */
	if (txn == NULL)
		return false;

	/*
	 * 记住预备信息，以便在我们跳过执行预备后供           
	 * 提交预备时使用。   
	 */
	txn->final_lsn = prepare_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.prepare_time = prepare_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	return true;
}

/* 记住我们已经跳过了预备     */
void
ReorderBufferSkipPrepare(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr, false);

	/* 未知事务，无事可做        */
	if (txn == NULL)
		return;

	txn->txn_flags |= RBTXN_SKIPPED_PREPARE;
}

/*
 * 预备一个两阶段事务。  
 *
 * 参见 ReorderBufferReplay()。        
 */
void
ReorderBufferPrepare(ReorderBuffer *rb, TransactionId xid,
					 char *gid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* 未知事务，没有可重放的内容*/
	if (txn == NULL)
		return;

	txn->txn_flags |= RBTXN_PREPARE;
	txn->gid = pstrdup(gid);

	/* 此时预备信息必定已在 txn 中完成更新。  */
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	ReorderBufferReplay(txn, rb, xid, txn->final_lsn, txn->end_lsn,
						txn->xact_time.prepare_time, txn->origin_id, txn->origin_lsn);

	/*
	 * 我们会为并发中止的事务发送预备，以便之后当     
	 * 解码并发送回滚预备时，下游能够回滚这样的        
	 * 事务。参见 DecodePrepare 顶部的注释。              
	 *
	 * 注意，对于 concurrent_abort + streaming 情况，一条        
	 * 已随上面 ReorderBufferReplay 调用发出。       
	 */
	if (txn->concurrent_abort && !rbtxn_is_streamed(txn))
		rb->prepare(rb, txn, txn->final_lsn);
}

/*
 * 这用于处理 COMMIT/ROLLBACK PREPARED。     
 */
void
ReorderBufferFinishPrepared(ReorderBuffer *rb, TransactionId xid,
							XLogRecPtr commit_lsn, XLogRecPtr end_lsn,
							XLogRecPtr two_phase_at,
							TimestampTz commit_time, RepOriginId origin_id,
							XLogRecPtr origin_lsn, char *gid, bool is_commit)
{
	ReorderBufferTXN *txn;
	XLogRecPtr	prepare_end_lsn;
	TimestampTz prepare_time;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, commit_lsn, false);

	/* 未知事务，无事可做        */
	if (txn == NULL)
		return;

	/*
	 * 此时该事务已拥有预备记录的信息，记住它以供        
	 * 之后回滚时使用。   
	 */
	prepare_end_lsn = txn->end_lsn;
	prepare_time = txn->xact_time.prepare_time;

	/* 把 gid 加入事务   */
	txn->gid = pstrdup(gid);

	/*
	 * 该事务有可能在预备时未被解码，原因可能是那    
	 * 时还没有一致的快照，或者 two_phase 尚未启用，      
	 * 又或者它先前已被解码但我们已经重启过。只    
	 * 有当它先前未被解码时才需要发送预备。如果     
	 * 尚未解码，那么对于中止我们无需再解码该事          
	 * 务。  
	 */
	if ((txn->final_lsn < two_phase_at) && is_commit)
	{
		txn->txn_flags |= RBTXN_PREPARE;

		/*
		 * 即便跳过 prepare，txn 中的 prepare 信息也必须     
		 * 更新  
		 */
		Assert(txn->final_lsn != InvalidXLogRecPtr);

		/*
		 * 此时该 txn 已拥有预备记录的信息，重要的是      
		 * 用它，让下游获得准确的信息。如果改为    
		 * 此处传入提交信息，下游可能会表现得就像      
		 * 自己在重启后已重放过 commit_prepared 一样。    
		 *                            
		 */
		ReorderBufferReplay(txn, rb, xid, txn->final_lsn, txn->end_lsn,
							txn->xact_time.prepare_time, txn->origin_id, txn->origin_lsn);
	}

	txn->final_lsn = commit_lsn;
	txn->end_lsn = end_lsn;
	txn->xact_time.commit_time = commit_time;
	txn->origin_id = origin_id;
	txn->origin_lsn = origin_lsn;

	if (is_commit)
		rb->commit_prepared(rb, txn, commit_lsn);
	else
		rb->rollback_prepared(rb, txn, prepare_end_lsn, prepare_time);

	/* 清理：确保没有缓存污染             */
	ReorderBufferExecuteInvalidations(txn->ninvalidations,
									  txn->invalidations);
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * 中止一个可能已有先前变更的事务。需要先对子          
 * 事务调用，然后再对顶层 XID 调用。          
 *
 * 注意：这里处理的事务必须已经主动中止（即产       
 * 生过中止记录）。隐式中止的事务经由                       
 * ReorderBufferAbortOld() 处理；我们不感兴趣但已              
 * 提交的事务由 ReorderBufferForget() 处理。        
 *
 * 此函数会将该事务及其内容从内存和磁盘中清除。    
 *      
 */
void
ReorderBufferAbort(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn,
				   TimestampTz abort_time)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* 未知，无需移除      */
	if (txn == NULL)
		return;

	txn->xact_time.abort_time = abort_time;

	/* 对于流式事务，向远程节点通知此次中止。         */
	if (rbtxn_is_streamed(txn))
	{
		rb->stream_abort(rb, txn, lsn);

		/*
		 * 我们可能已经为该事务解码了一些变更，它们会   
		 * 按当前事务的视图加载缓存（设想该事务中      
		 * 发生过的 DDL）。我们不希望未来事务的解码使用   
		 * 那些缓存条目，因此只执行该事务中的失效消息  
		 *                              
		 */
		if (txn->ninvalidations > 0)
			ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
											   txn->invalidations);
	}

	/* 仅为美化*/
	txn->final_lsn = lsn;

	/* 移除可能的磁盘数据并释放内存    */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * 中止所有因服务器重启而实际上已不再运行的事        
 * 务。           
 *
 * 注意：这些必须确实是因服务器崩溃/立即重启             
 * 而中止的事务，因为我们在此不处理失效消息。   
 */
void
ReorderBufferAbortOld(ReorderBuffer *rb, TransactionId oldestRunningXid)
{
	dlist_mutable_iter it;

	/*
	 * 遍历所有（可能的）顶层 TXN，并中止所有那些       
	 * 比可能仍在运行的更早的事务。一旦发现第一个    
	 * 仍然存活的事务就会停止，可能有些事务更早获        
	 * 得了 XID 却较晚才开始写入，但这不太可能，会       
	 * 在后续调用中被清理掉。   
	 */
	dlist_foreach_modify(it, &rb->toplevel_by_lsn)
	{
		ReorderBufferTXN *txn;

		txn = dlist_container(ReorderBufferTXN, node, it.cur);

		if (TransactionIdPrecedes(txn->xid, oldestRunningXid))
		{
			elog(DEBUG2, "aborting old transaction %u", txn->xid);

			/* 就崩溃/立即重启通知远程节点。               */
			if (rbtxn_is_streamed(txn))
				rb->stream_abort(rb, txn, InvalidXLogRecPtr);

			/* 移除可能的磁盘数据并释放此事务         */
			ReorderBufferCleanupTXN(rb, txn);
		}
		else
			return;
	}
}

/*
 * 如果我们对某事务的内容不感兴趣，则忘记其       
 * 内容。需要先对子事务调用，然后再对顶层 XID          
 * 调用。    
 *
 * 这与 ReorderBufferAbort() 有显著的不同，因为        
 * 已提交的事务需要与已中止的事务区别对待，因             
 * 为它们可能已经修改过系统表。    
 *
 * 注意，只允许在刚刚读取到事务提交记录的那一刻     
 * 调用此函数，而无法更早；否则，之后引用此             
 * XID 的记录可能会将该事务重建得不完整。  
 */
void
ReorderBufferForget(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* 未知，无需忘记      */
	if (txn == NULL)
		return;

	/* 此事务不允许被流式发送    */
	Assert(!rbtxn_is_streamed(txn));

	/* 仅为美化*/
	txn->final_lsn = lsn;

	/*
	 * 此事务中若有缓存失效消息，则只处理它们。         
	 * 即使我们对事务的内容不感兴趣，它也可能              
	 * 修改过系统表，我们需要据此                            
	 * 更新缓存。   
	 */
	if (txn->base_snapshot != NULL && txn->ninvalidations > 0)
		ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
										   txn->invalidations);
	else
		Assert(txn->ninvalidations == 0);

	/* 删除可能存在的磁盘数据并释放    */
	ReorderBufferCleanupTXN(rb, txn);
}

/*
 * 对需要跳过的那些事务也使缓存失效，以防系统表作为    
 * 事务的一部分而被修改过。                 
 *
 * 注意，这是一个针对已准备事务的专用函数，即使我们    
 * 决定跳过它，也不希望清理 TXN。参见 DecodePrepare。   
 *               
 */
void
ReorderBufferInvalidate(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	/* 未知，无需处理  */
	if (txn == NULL)
		return;

	/*
	 * 若存在缓存失效消息，则处理它们。即使我们对事务  
	 * 的内容不感兴趣，它也可能修改过系统表，我们需要  
	 * 据此更新缓存。                                      
	 */
	if (txn->base_snapshot != NULL && txn->ninvalidations > 0)
		ReorderBufferImmediateInvalidation(rb, txn->ninvalidations,
										   txn->invalidations);
	else
		Assert(txn->ninvalidations == 0);
}


/*
 * 执行已解码事务上下文之外发生的失效。          
 * 目前这只发生在两类情形：无 XID 的提交（参见 
 * RecordTransactionCommit()），以及不感兴趣事务中的失效  
 * （经由 ReorderBufferForget()）。    
 */
void
ReorderBufferImmediateInvalidation(ReorderBuffer *rb, uint32 ninvalidations,
								   SharedInvalidationMessage *invalidations)
{
	bool		use_subtxn = IsTransactionOrTransactionBlock();
	int			i;

	if (use_subtxn)
		BeginInternalSubTransaction("replay");

	/*
	 * 强制使失效发生在有效事务之外——这样条目只会     
	 * 被标记为失效，无需访问系统表。这样做的好处      
	 * 是我们无需设置访问系统表所需的                    
	 * 完整状态。              
	 */
	if (use_subtxn)
		AbortCurrentTransaction();

	for (i = 0; i < ninvalidations; i++)
		LocalExecuteInvalidationMessage(&invalidations[i]);

	if (use_subtxn)
		RollbackAndReleaseCurrentSubTransaction();
}

/*
 * 将 WAL 流中看到的 xid 告知重排序缓冲区。对于              
 * XLogRecord->xl_xid 中的每个 xid 都必须至少调用一次（     
 * 其他位置也可能经过，非必须）。   
 *
 * 出于效率考虑，重排序缓冲区按 LSN 顺序保存事务       
 * 的相关数据结构。为此它必须知道事务在 WAL 中            
 * 首次出现的时间。由于许多类型的记录对逻辑解码来说   
 * 不真正重要，因此不一定会经过这里。        
 */
void
ReorderBufferProcessXid(ReorderBuffer *rb, TransactionId xid, XLogRecPtr lsn)
{
	/* 许多记录没有分配 xid，把该检查集中到这里     */
	if (xid != InvalidTransactionId)
		ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);
}

/*
 * 向此事务添加一个新快照，仅在 lsn 'lsn' 后方可           
 * 使用，因为上个快照无法为后续行正确描述系统表。   
 *                
 */
void
ReorderBufferAddSnapshot(ReorderBuffer *rb, TransactionId xid,
						 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);

	change->data.snapshot = snap;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT;

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * 设置事务的基础快照。         
 *
 * 若已知 xid 是子事务，则改为在顶层事务上             
 * 设置基础快照。         
 */
void
ReorderBufferSetBaseSnapshot(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, Snapshot snap)
{
	ReorderBufferTXN *txn;
	bool		is_new;

	Assert(snap != NULL);

	/*
	 * 获取要操作的事务。如果我们知道它是一个子事务，  
	 * 则改为操作其顶层事务。            
	 */
	txn = ReorderBufferTXNByXid(rb, xid, true, &is_new, lsn, true);
	if (rbtxn_is_known_subxact(txn))
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);
	Assert(txn->base_snapshot == NULL);

	txn->base_snapshot = snap;
	txn->base_snapshot_lsn = lsn;
	dlist_push_tail(&rb->txns_by_base_snapshot_lsn, &txn->base_snapshot_node);

	AssertTXNLsnOrder(rb);
}

/*
 * 在变更流的这个位置以该 CommandId 访问系统表。           
 *
 * 只能为 > 1 的命令 ID 调用     
 */
void
ReorderBufferAddNewCommandId(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, CommandId cid)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);

	change->data.command_id = cid;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID;

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * 更新内存计数器以计入新增或移除的变更。       
 *
 * 我们更新两个计数器：重排序缓冲区中的计数器和    
 * 包含该变更的事务中的计数器。重排序缓冲区计数器 
 * 我们快速判断是否达到内存限制；事务计数器则让   
 * 我们能快速挑选出最大的事务以供逐出。  
 *
 * 启用流式时，需要改为更新顶层事务的计数器：      
 * 我们并不真正关心子事务，反正无法单独进行流式 
 * 处理，而且我们只会挑选顶层事务进行逐出。    
 * 所以只有顶层事务才重要。                            
 */
static void
ReorderBufferChangeMemoryUpdate(ReorderBuffer *rb,
								ReorderBufferChange *change,
								bool addition, Size sz)
{
	ReorderBufferTXN *txn;
	ReorderBufferTXN *toptxn;

	Assert(change->txn);

	/*
	 * 忽略元组 CID 变更，因为达到内存限制时它们不会    
	 * 被逐出。所以干脆不对它们计数，因为这            
	 * 容易触发无谓的溢写尝试。 
	 */
	if (change->action == REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID)
		return;

	txn = change->txn;

	/*
	 * 同时更新顶层中的总大小。此值之后将用于        
	 * 计算解码统计。      
	 */
	toptxn = rbtxn_get_toptxn(txn);

	if (addition)
	{
		txn->size += sz;
		rb->size += sz;

		/* 更新顶层事务中的总大小。          */
		toptxn->total_size += sz;
	}
	else
	{
		Assert((rb->size >= sz) && (txn->size >= sz));
		txn->size -= sz;
		rb->size -= sz;

		/* 更新顶层事务中的总大小。          */
		toptxn->total_size -= sz;
	}

	Assert(txn->size <= rb->size);
}

/*
 * 新增 (relfilelocator, tid) -> (cmin, cmax) 映射    
 *
 * 此变更类型不计入内存核算，因为 CID 保存在单独  
 * 的列表中，达到内存限制时不会逐出它们。       
 *                  
 */
void
ReorderBufferAddNewTupleCids(ReorderBuffer *rb, TransactionId xid,
							 XLogRecPtr lsn, RelFileLocator locator,
							 ItemPointerData tid, CommandId cmin,
							 CommandId cmax, CommandId combocid)
{
	ReorderBufferChange *change = ReorderBufferGetChange(rb);
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	change->data.tuplecid.locator = locator;
	change->data.tuplecid.tid = tid;
	change->data.tuplecid.cmin = cmin;
	change->data.tuplecid.cmax = cmax;
	change->data.tuplecid.combocid = combocid;
	change->lsn = lsn;
	change->txn = txn;
	change->action = REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID;

	dlist_push_tail(&txn->tuplecids, &change->node);
	txn->ntuplecids++;
}

/*
 * 向重排序缓冲区队列添加新的失效消息。    
 */
static void
ReorderBufferQueueInvalidations(ReorderBuffer *rb, TransactionId xid,
								XLogRecPtr lsn, Size nmsgs,
								SharedInvalidationMessage *msgs)
{
	ReorderBufferChange *change;

	change = ReorderBufferGetChange(rb);
	change->action = REORDER_BUFFER_CHANGE_INVALIDATION;
	change->data.inval.ninvalidations = nmsgs;
	change->data.inval.invalidations = (SharedInvalidationMessage *)
		palloc(sizeof(SharedInvalidationMessage) * nmsgs);
	memcpy(change->data.inval.invalidations, msgs,
		   sizeof(SharedInvalidationMessage) * nmsgs);

	ReorderBufferQueueChange(rb, xid, lsn, change, false);
}

/*
 * 辅助函数：供 ReorderBufferAddInvalidations() 和   
 * ReorderBufferAddDistributedInvalidations() 将失效消息累积到      
 * **invals_out。              
 */
static void
ReorderBufferAccumulateInvalidations(SharedInvalidationMessage **invals_out,
									 uint32 *ninvals_out,
									 SharedInvalidationMessage *msgs_new,
									 Size nmsgs_new)
{
	if (*ninvals_out == 0)
	{
		*ninvals_out = nmsgs_new;
		*invals_out = (SharedInvalidationMessage *)
			palloc(sizeof(SharedInvalidationMessage) * nmsgs_new);
		memcpy(*invals_out, msgs_new, sizeof(SharedInvalidationMessage) * nmsgs_new);
	}
	else
	{
		/* 扩大失效消息数组            */
		*invals_out = (SharedInvalidationMessage *)
			repalloc(*invals_out, sizeof(SharedInvalidationMessage) *
					 (*ninvals_out + nmsgs_new));
		memcpy(*invals_out + *ninvals_out, msgs_new,
			   nmsgs_new * sizeof(SharedInvalidationMessage));
		*ninvals_out += nmsgs_new;
	}
}

/*
 * 累积这些失效消息以便稍后执行。         
 *
 * 需要针对每条 XLOG_XACT_INVALIDATIONS 消息调用本函数， 
 * 它会把所有失效消息累积到顶层事务（若有），否则    
 * 累积到当前事务中，同时也以变更形式累积到重排序     
 * 缓冲区中。需要以变更的形式记录它，是为了在每次   
 * CommandId 递增时只执行所需的失效，而不是执行全部        
 * 失效。还需把这些累积到事务缓冲区，因为在某些 
 * 情况下跳过对事务的处理（参见 ReorderBufferForget），  
 * 此时需要把所有失效一起执行。                              
 *                                
 */
void
ReorderBufferAddInvalidations(ReorderBuffer *rb, TransactionId xid,
							  XLogRecPtr lsn, Size nmsgs,
							  SharedInvalidationMessage *msgs)
{
	ReorderBufferTXN *txn;
	MemoryContext oldcontext;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/*
	 * 将所有失效收集到顶层事务（若有）下，以便一起    
	 * 执行它们。参见本函数顶部的注释。                 
	 *          
	 */
	txn = rbtxn_get_toptxn(txn);

	Assert(nmsgs > 0);

	ReorderBufferAccumulateInvalidations(&txn->invalidations,
										 &txn->ninvalidations,
										 msgs, nmsgs);

	ReorderBufferQueueInvalidations(rb, xid, lsn, nmsgs, msgs);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 累积其他已提交事务分发的失效，以便稍后执行。      
 *                          
 *
 * 本函数与 ReorderBufferAddInvalidations() 类似，但会把给定 
 * 的失效消息存储到 txn->invalidations_distributed，并会检查 
 * 溢出。      
 *
 * 已提交的事务需调用它，以便把自身失效消息分发   
 * 给正在进行的事务。                
 */
void
ReorderBufferAddDistributedInvalidations(ReorderBuffer *rb, TransactionId xid,
										 XLogRecPtr lsn, Size nmsgs,
										 SharedInvalidationMessage *msgs)
{
	ReorderBufferTXN *txn;
	MemoryContext oldcontext;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/*
	 * 将所有失效收集到顶层事务（若有），以便一起执行 
	 * 它们。参见 ReorderBufferAddInvalidations。       
	 *                               
	 */
	txn = rbtxn_get_toptxn(txn);

	Assert(nmsgs > 0);

	if (!rbtxn_distr_inval_overflowed(txn))
	{
		/*
		 * 检查事务是否有足够空间存储分发的失效消息  
		 *                       
		 */
		if (txn->ninvalidations_distributed + nmsgs >= MAX_DISTR_INVAL_MSG_PER_TXN)
		{
			/*
			 * 将失效消息标记为已溢出，并释放已累积的  
			 * 全部消息。             
			 */
			txn->txn_flags |= RBTXN_DISTR_INVAL_OVERFLOWED;

			if (txn->invalidations_distributed)
			{
				pfree(txn->invalidations_distributed);
				txn->invalidations_distributed = NULL;
				txn->ninvalidations_distributed = 0;
			}
		}
		else
			ReorderBufferAccumulateInvalidations(&txn->invalidations_distributed,
												 &txn->ninvalidations_distributed,
												 msgs, nmsgs);
	}

	/* 将失效消息排入该事务的队列              */
	ReorderBufferQueueInvalidations(rb, xid, lsn, nmsgs, msgs);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * 应用我们已知的全部失效。在变更流的此处可能只需     
 * 其中一部分，但我们并不知道是哪些。   
 */
static void
ReorderBufferExecuteInvalidations(uint32 nmsgs, SharedInvalidationMessage *msgs)
{
	int			i;

	for (i = 0; i < nmsgs; i++)
		LocalExecuteInvalidationMessage(&msgs[i]);
}

/*
 * 将事务标记为包含系统表变更         
 */
void
ReorderBufferXidSetCatalogChanges(ReorderBuffer *rb, TransactionId xid,
								  XLogRecPtr lsn)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, true, NULL, lsn, true);

	if (!rbtxn_has_catalog_changes(txn))
	{
		txn->txn_flags |= RBTXN_HAS_CATALOG_CHANGES;
		dclist_push_tail(&rb->catchange_txns, &txn->catchange_node);
	}

	/*
	 * 若某个子事务有系统表变更，也把顶层事务标记为有 
	 * 系统表变更，这样 ReorderBufferBuildTupleCidHash    
	 * 就能方便地只检查顶层事务，并决定是否构建       
	 * 哈希表。                
	 */
	if (rbtxn_is_subtxn(txn))
	{
		ReorderBufferTXN *toptxn = rbtxn_get_toptxn(txn);

		if (!rbtxn_has_catalog_changes(toptxn))
		{
			toptxn->txn_flags |= RBTXN_HAS_CATALOG_CHANGES;
			dclist_push_tail(&rb->catchange_txns, &toptxn->catchange_node);
		}
	}
}

/*
 * 返回已更改系统表的事务的 palloc 数组，该数组按     
 * xidComparator 顺序排序。                       
 *
 * 用完后调用者必须释放返回的数组。          
 */
TransactionId *
ReorderBufferGetCatalogChangesXacts(ReorderBuffer *rb)
{
	dlist_iter	iter;
	TransactionId *xids = NULL;
	size_t		xcnt = 0;

	/* 若列表为空则快速返回    */
	if (dclist_count(&rb->catchange_txns) == 0)
		return NULL;

	/* 初始化 XID 数组 */
	xids = (TransactionId *) palloc(sizeof(TransactionId) *
									dclist_count(&rb->catchange_txns));
	dclist_foreach(iter, &rb->catchange_txns)
	{
		ReorderBufferTXN *txn = dclist_container(ReorderBufferTXN,
												 catchange_node,
												 iter.cur);

		Assert(rbtxn_has_catalog_changes(txn));

		xids[xcnt++] = txn->xid;
	}

	qsort(xids, xcnt, sizeof(TransactionId), xidComparator);

	Assert(xcnt == dclist_count(&rb->catchange_txns));
	return xids;
}

/*
 * 查询某事务是否已*已知*包含系统表变更。         
 * 在紧邻提交之前，它都可能是错的！            
 */
bool
ReorderBufferXidHasCatalogChanges(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);
	if (txn == NULL)
		return false;

	return rbtxn_has_catalog_changes(txn);
}

/*
 * ReorderBufferXidHasBaseSnapshot
 *		是否已为给定的 txn/子事务设置基础快照？       
 */
bool
ReorderBufferXidHasBaseSnapshot(ReorderBuffer *rb, TransactionId xid)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false,
								NULL, InvalidXLogRecPtr, false);

	/* 事务尚未知，因此没有快照          */
	if (txn == NULL)
		return false;

	/* 已知的子事务？改为操作顶层事务    */
	if (rbtxn_is_known_subxact(txn))
		txn = ReorderBufferTXNByXid(rb, txn->toplevel_xid, false,
									NULL, InvalidXLogRecPtr, false);

	return txn->base_snapshot != NULL;
}


/*
 * ---------------------------------------
 * 磁盘序列化支持     
 * ---------------------------------------
 */

/*
 * 确保 IO 缓冲区 >= sz。  
 */
static void
ReorderBufferSerializeReserve(ReorderBuffer *rb, Size sz)
{
	if (!rb->outbufsize)
	{
		rb->outbuf = MemoryContextAlloc(rb->context, sz);
		rb->outbufsize = sz;
	}
	else if (rb->outbufsize < sz)
	{
		rb->outbuf = repalloc(rb->outbuf, sz);
		rb->outbufsize = sz;
	}
}

/*
 * 找出要溢写到磁盘来逐出的最大事务（顶层/子事务）。   
 *
 * XXX 子事务很多时该过程可能相当慢，因为必须遍历它们 
 * 。改进思路：(a) 维护按变更数量排序的辅助结构，     
 * (b) 不寻找完全最大的事务，而是寻找至少占用了       
 * 内存限制一定比例的事务；以及 (c) 一次逐出多个事务 
 * ，例如释放内存限制的给定比例（例如 50%）。             
 *                                                                             
 *                                
 */
static ReorderBufferTXN *
ReorderBufferLargestTXN(ReorderBuffer *rb)
{
	HASH_SEQ_STATUS hash_seq;
	ReorderBufferTXNByIdEnt *ent;
	ReorderBufferTXN *largest = NULL;

	hash_seq_init(&hash_seq, rb->by_txn);
	while ((ent = hash_seq_search(&hash_seq)) != NULL)
	{
		ReorderBufferTXN *txn = ent->txn;

		/* 若当前事务更大，则记住它              */
		if ((!largest) || (txn->size > largest->size))
			largest = txn;
	}

	Assert(largest);
	Assert(largest->size > 0);
	Assert(largest->size <= rb->size);

	return largest;
}

/*
 * 找出要以流式方式逐出的最大可流式顶层事务。          
 *
 * 这可以看作 ReorderBufferLargestTXN 的优化版本，后者应该会 
 * 给出相同的事务（因为流式下不更新子事务的内存         
 * 账目，其值总是 0）。但我们只需遍历有限的、拥      
 * 有基础快照的顶层事务即可。挑选没有基础快照的事务 
 * 毫无意义，因为我们不会解码这类事务。此外，我们也   
 * 不会选择没有任何可流式变更的事务。                        
 *                                                          
 *
 * 注意，我们会跳过含有不完整变更的事务。这里存在  
 * 优化空间：可选出含不完整变更的最大事务。但    
 * 这会让代码和设计变得相当复杂，可能不值得。若我们  
 * 计划对包含不完整变更的事务进行流式处理，就需要找    
 * 到在内存中部分流式/截断事务变更的办法，并构建     
 * 部分截断溢写文件的机制。此外，每当对事务进行部分 
 * 流式处理时需维护上次流式传输的 lsn，下次    
 * 需要从该段以及 WAL 中的偏移量恢复。                   
 * 由于我们从顶层事务流式传输变更、并按子事务逐一    
 * 恢复，甚至还要记住我们流式传输最后一个变更时   
 * 所在的那个子事务。                                               
 *                                                    
 */
static ReorderBufferTXN *
ReorderBufferLargestStreamableTopTXN(ReorderBuffer *rb)
{
	dlist_iter	iter;
	Size		largest_size = 0;
	ReorderBufferTXN *largest = NULL;

	/* 找出拥有基础快照的最大顶层事务。               */
	dlist_foreach(iter, &rb->txns_by_base_snapshot_lsn)
	{
		ReorderBufferTXN *txn;

		txn = dlist_container(ReorderBufferTXN, base_snapshot_node, iter.cur);

		/* 不允许为子事务*/
		Assert(!rbtxn_is_known_subxact(txn));
		/* 已设置 base_snapshot   */
		Assert(txn->base_snapshot != NULL);

		if ((largest == NULL || txn->total_size > largest_size) &&
			(txn->total_size > 0) && !(rbtxn_has_partial_change(txn)) &&
			rbtxn_has_streamable_change(txn))
		{
			largest = txn;
			largest_size = txn->total_size;
		}
	}

	return largest;
}

/*
 * 检查是否达到 logical_decoding_work_mem 限制，若达到，就    
 * 每次选出一个最大的（子）事务来逐出，将其变更溢写到  
 * 磁盘或发送给输出插件，直到降到内存限制以下。      
 *
 * 如果 debug_logical_replication_streaming 设置为 "immediate"，    
 * 立即流式或序列化变更。 
 *
 * XXX 在这里我们会持续选出事务，直到降到内存限制         
 * 以下，但我们也可以采用一种更精细的逐出策略——例如    
 * 逐出足够多的事务，释放内存限制的某比例（例如 50%）。  
 *       
 */
static void
ReorderBufferCheckMemoryLimit(ReorderBuffer *rb)
{
	ReorderBufferTXN *txn;

	/*
	 * 如果 debug_logical_replication_streaming 为 buffered 且尚未 
	 * 超过内存限制即返回。    
	 */
	if (debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_BUFFERED &&
		rb->size < logical_decoding_work_mem * 1024L)
		return;

	/*
	 * 如果 debug_logical_replication_streaming 为 "immediate"，则循环 
	 * 到没有变更为止。否则，就循环到内存限制以下。   
	 * 有人或许认为，只逐出最大的（子）事务就能降到    
	 * 内存限制以下，假设是所选事务至少与最近变更  
	 * （正是它使我们超限）一样大。事实并非如此，  
	 * 因为用户可能在最近的变更之前，就已把           
	 * logical_decoding_work_mem 调小。                                 
	 *                                     
	 */
	while (rb->size >= logical_decoding_work_mem * 1024L ||
		   (debug_logical_replication_streaming == DEBUG_LOGICAL_REP_STREAMING_IMMEDIATE &&
			rb->size > 0))
	{
		/*
		 * 选出最大的事务，若可行则以流式从内存中逐出， 
		 * 否则将其溢写到磁盘。         
		 */
		if (ReorderBufferCanStartStreaming(rb) &&
			(txn = ReorderBufferLargestStreamableTopTXN(rb)) != NULL)
		{
			/* 我们知道一定存在一个，因为大小不为零    */
			Assert(txn && rbtxn_is_toptxn(txn));
			Assert(txn->total_size > 0);
			Assert(rb->size >= txn->total_size);

			ReorderBufferStreamTXN(rb, txn);
		}
		else
		{
			/*
			 * 选出最大事务（或子事务），序列化到磁盘以 
			 * 将其从内存中逐出。           
			 */
			txn = ReorderBufferLargestTXN(rb);

			/* 我们知道一定存在一个，因为大小不为零    */
			Assert(txn);
			Assert(txn->size > 0);
			Assert(rb->size >= txn->size);

			ReorderBufferSerializeTXN(rb, txn);
		}

		/*
		 * 逐出后，该事务在内存中应没有任何条目，且所  
		 * 用 0 字节存储变更。        
		 */
		Assert(txn->size == 0);
		Assert(txn->nentries_mem == 0);
	}

	/* 现在必须低于内存限制。      */
	Assert(rb->size < logical_decoding_work_mem * 1024L);
}

/*
 * 把大事务（及其子事务）的数据溢写到磁盘。        
 */
static void
ReorderBufferSerializeTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	dlist_iter	subtxn_i;
	dlist_mutable_iter change_i;
	int			fd = -1;
	XLogSegNo	curOpenSegNo = 0;
	Size		spilled = 0;
	Size		size = txn->size;

	elog(DEBUG2, "spill %u changes in XID %u to disk",
		 (uint32) txn->nentries_mem, txn->xid);

	/* 对子事务做同样处理  */
	dlist_foreach(subtxn_i, &txn->subtxns)
	{
		ReorderBufferTXN *subtxn;

		subtxn = dlist_container(ReorderBufferTXN, node, subtxn_i.cur);
		ReorderBufferSerializeTXN(rb, subtxn);
	}

	/* 序列化变更流     */
	dlist_foreach_modify(change_i, &txn->changes)
	{
		ReorderBufferChange *change;

		change = dlist_container(ReorderBufferChange, node, change_i.cur);

		/*
		 * 按起始 lsn 存入所属的段，不过不要跨多个           
		 * 段拆分它。      
		 */
		if (fd == -1 ||
			!XLByteInSeg(change->lsn, curOpenSegNo, wal_segment_size))
		{
			char		path[MAXPGPATH];

			if (fd != -1)
				CloseTransientFile(fd);

			XLByteToSeg(change->lsn, curOpenSegNo, wal_segment_size);

			/*
			 * 无需关心 TLI，仅用于单次运行，每个 LSN 只      
			 * 映射到特定 WAL 记录。                  
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										curOpenSegNo);

			/* 打开段，必要时创建          */
			fd = OpenTransientFile(path,
								   O_CREAT | O_WRONLY | O_APPEND | PG_BINARY);

			if (fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", path)));
		}

		ReorderBufferSerializeChange(rb, txn, fd, change);
		dlist_delete(&change->node);
		ReorderBufferReturnChange(rb, change, true);

		spilled++;
	}

	/* 若溢写过任何内容则更新统计信息      */
	if (spilled)
	{
		rb->spillCount += 1;
		rb->spillBytes += size;

		/* 不考虑已序列化的事务                 */
		rb->spillTxns += (rbtxn_is_serialized(txn) || rbtxn_is_serialized_clear(txn)) ? 0 : 1;

		/* 更新解码统计信息  */
		UpdateDecodingStats((LogicalDecodingContext *) rb->private_data);
	}

	Assert(spilled == txn->nentries_mem);
	Assert(dlist_is_empty(&txn->changes));
	txn->nentries_mem = 0;
	txn->txn_flags |= RBTXN_IS_SERIALIZED;

	if (fd != -1)
		CloseTransientFile(fd);
}

/*
 * 将单个变更序列化到磁盘   
 */
static void
ReorderBufferSerializeChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
							 int fd, ReorderBufferChange *change)
{
	ReorderBufferDiskChange *ondisk;
	Size		sz = sizeof(ReorderBufferDiskChange);

	ReorderBufferSerializeReserve(rb, sz);

	ondisk = (ReorderBufferDiskChange *) rb->outbuf;
	memcpy(&ondisk->change, change, sizeof(ReorderBufferChange));

	switch (change->action)
	{
			/* 这些分支直接贯穿，彼此足够相似  */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			{
				char	   *data;
				ReorderBufferTupleBuf *oldtup,
						   *newtup;
				Size		oldlen = 0;
				Size		newlen = 0;

				oldtup = change->data.tp.oldtuple;
				newtup = change->data.tp.newtuple;

				if (oldtup)
				{
					sz += sizeof(HeapTupleData);
					oldlen = oldtup->tuple.t_len;
					sz += oldlen;
				}

				if (newtup)
				{
					sz += sizeof(HeapTupleData);
					newlen = newtup->tuple.t_len;
					sz += newlen;
				}

				/* 确保有足够的空间       */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* 可能已在上文重新分配    */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				if (oldlen)
				{
					memcpy(data, &oldtup->tuple, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, oldtup->tuple.t_data, oldlen);
					data += oldlen;
				}

				if (newlen)
				{
					memcpy(data, &newtup->tuple, sizeof(HeapTupleData));
					data += sizeof(HeapTupleData);

					memcpy(data, newtup->tuple.t_data, newlen);
					data += newlen;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				char	   *data;
				Size		prefix_size = strlen(change->data.msg.prefix) + 1;

				sz += prefix_size + change->data.msg.message_size +
					sizeof(Size) + sizeof(Size);
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);

				/* 可能已在上文重新分配    */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				/* 写入前缀及其大小            */
				memcpy(data, &prefix_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.prefix,
					   prefix_size);
				data += prefix_size;

				/* 写入消息及其大小             */
				memcpy(data, &change->data.msg.message_size, sizeof(Size));
				data += sizeof(Size);
				memcpy(data, change->data.msg.message,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				char	   *data;
				Size		inval_size = sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;

				sz += inval_size;

				ReorderBufferSerializeReserve(rb, sz);
				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);

				/* 可能已在上文重新分配    */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;
				memcpy(data, change->data.inval.invalidations, inval_size);
				data += inval_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	snap;
				char	   *data;

				snap = change->data.snapshot;

				sz += sizeof(SnapshotData) +
					sizeof(TransactionId) * snap->xcnt +
					sizeof(TransactionId) * snap->subxcnt;

				/* 确保有足够的空间       */
				ReorderBufferSerializeReserve(rb, sz);
				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* 可能已在上文重新分配    */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, snap, sizeof(SnapshotData));
				data += sizeof(SnapshotData);

				if (snap->xcnt)
				{
					memcpy(data, snap->xip,
						   sizeof(TransactionId) * snap->xcnt);
					data += sizeof(TransactionId) * snap->xcnt;
				}

				if (snap->subxcnt)
				{
					memcpy(data, snap->subxip,
						   sizeof(TransactionId) * snap->subxcnt);
					data += sizeof(TransactionId) * snap->subxcnt;
				}
				break;
			}
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Size		size;
				char	   *data;

				/* 计入被截断关系的 OID                */
				size = sizeof(Oid) * change->data.truncate.nrelids;
				sz += size;

				/* 确保有足够的空间       */
				ReorderBufferSerializeReserve(rb, sz);

				data = ((char *) rb->outbuf) + sizeof(ReorderBufferDiskChange);
				/* 可能已在上文重新分配    */
				ondisk = (ReorderBufferDiskChange *) rb->outbuf;

				memcpy(data, change->data.truncate.relids, size);
				data += size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			/* ReorderBufferChange 包含了所有重要信息   */
			break;
	}

	ondisk->size = sz;

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_REORDER_BUFFER_WRITE);
	if (write(fd, rb->outbuf, ondisk->size) != ondisk->size)
	{
		int			save_errno = errno;

		CloseTransientFile(fd);

		/* 若写入未设置 errno，则假定磁盘空间不足     */
		errno = save_errno ? save_errno : ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to data file for XID %u: %m",
						txn->xid)));
	}
	pgstat_report_wait_end();

	/*
	 * 把每条变更发送到磁盘时，要让事务的 final_lsn 保持   
	 * 最新，这样 ReorderBufferRestoreCleanup 才能正常工作。（   
	 * 去我们只在提交和中止记录上这么做，但如果系统  
	 * 崩溃使事务无其中止记录，就行不通了）。   
	 *
	 * 务必不要把它往回移。     
	 */
	if (txn->final_lsn < change->lsn)
		txn->final_lsn = change->lsn;

	Assert(ondisk->change.action == change->action);
}

/* 若输出插件支持流式则返回 true，否则返回 false。        */
static inline bool
ReorderBufferCanStream(ReorderBuffer *rb)
{
	LogicalDecodingContext *ctx = rb->private_data;

	return ctx->streaming;
}

/* 若现在可以开始流式则返回 true，否则返回 false。    */
static inline bool
ReorderBufferCanStartStreaming(ReorderBuffer *rb)
{
	LogicalDecodingContext *ctx = rb->private_data;
	SnapBuild  *builder = ctx->snapshot_builder;

	/* 在到达一致状态之前无法开始流式。               */
	if (SnapBuildCurrentState(builder) < SNAPBUILD_CONSISTENT)
		return false;

	/*
	 * 即使已启用流式，也无法立即开始流式，因为此前   
	 * 已经解码过该事务，而现在只是重新开始。      
	 *            
	 */
	if (ReorderBufferCanStream(rb) &&
		!SnapBuildXactNeedsSkip(builder, ctx->reader->ReadRecPtr))
		return true;

	return false;
}

/*
 * 把大事务（及其子事务）数据发送给输出插件，  
 * 但要使用流式 API。               
 */
static void
ReorderBufferStreamTXN(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	Snapshot	snapshot_now;
	CommandId	command_id;
	Size		stream_bytes;
	bool		txn_is_streamed;

	/* 子事务永远不可能走到这里。       */
	Assert(rbtxn_is_toptxn(txn));

	/*
	 * 在这里我们无法对基础快照做任何假设，这与           
	 * ReorderBufferCommit() 的做法类似。后者依赖于在      
	 * ReorderBufferCommitChild() 中把 base_snapshot 从子事务转移  
	 * 过来，但事务进行中，尚未调用它。 
	 *
	 * 所以这里只需遍历各子事务并沿用相同逻辑。但我们  
	 * 只需在事务首次被流式时执行一次，之后需要复用   
	 * 上一轮运行时的快照。                                 
	 *
	 * 与 DecodeCommit 不同——它用 SnapBuildCommitTxn 将所    
	 * 有子事务的 xid 加入快照的 xip 数组，这里无法这样   
	 * 做，而是通过 ReorderBufferCopySnap 把它们加入 subxip       
	 * 数组。这样，迄今已解码的子事务所做的系统表变更 
	 * 可见。  
	 */
	if (txn->snapshot_now == NULL)
	{
		dlist_iter	subxact_i;

		/* 确保该事务是第一次被流式                      */
		Assert(!rbtxn_is_streamed(txn));

		/* 一开始应当是无效的命令 ID               */
		Assert(txn->command_id == InvalidCommandId);

		dlist_foreach(subxact_i, &txn->subtxns)
		{
			ReorderBufferTXN *subtxn;

			subtxn = dlist_container(ReorderBufferTXN, node, subxact_i.cur);
			ReorderBufferTransferSnapToParent(txn, subtxn);
		}

		/*
		 * 若该事务没有快照，说明它至今未对数据库进行   
		 * 过任何修改，所以没有内容可解码。    
		 */
		if (txn->base_snapshot == NULL)
		{
			Assert(txn->ninvalidations == 0);
			return;
		}

		command_id = FirstCommandId;
		snapshot_now = ReorderBufferCopySnap(rb, txn->base_snapshot,
											 txn, command_id);
	}
	else
	{
		/* 该事务必然已被流式过                  */
		Assert(rbtxn_is_streamed(txn));

		/*
		 * 不必，我们已有上一轮流式运行时的快照。我们  
		 * 假定新子事务无法把 LSN 往回移，因此也无法满足  
		 * 上一分支的 LSN 条件（无需再遍历子事务）    
		 * 。事实上，我们根本不允许这样做，因为            
		 * 可能正在子事务中途使用快照。  
		 */
		command_id = txn->command_id;

		/*
		 * 我们无法直接使用 txn->snapshot_now，因为           
		 * 在上次流式运行后，可能得到新的子事务，因此  
		 * 需要将它们加入快照。   
		 */
		snapshot_now = ReorderBufferCopySnap(rb, txn->snapshot_now,
											 txn, command_id);

		/* 释放之前复制的快照。       */
		Assert(txn->snapshot_now->copied);
		ReorderBufferFreeSnap(rb, txn->snapshot_now);
		txn->snapshot_now = NULL;
	}

	/*
	 * 记住这些信息，稍后用于更新统计信息。我们无法  
	 * 在此更新统计信息，因为处理变更时发生错误会导  
	 * 致统计信息累积，尽管我们尚未流式发送全部         
	 * 变更。   
	 */
	txn_is_streamed = rbtxn_is_streamed(txn);
	stream_bytes = txn->total_size;

	/* 处理变更并将其发送给输出插件。  */
	ReorderBufferProcessTXN(rb, txn, InvalidXLogRecPtr, snapshot_now,
							command_id, true);

	rb->streamCount += 1;
	rb->streamBytes += stream_bytes;

	/* 不考虑已流式发送的事务。         */
	rb->streamTxns += (txn_is_streamed) ? 0 : 1;

	/* 更新解码统计信息  */
	UpdateDecodingStats((LogicalDecodingContext *) rb->private_data);

	Assert(dlist_is_empty(&txn->changes));
	Assert(txn->nentries == 0);
	Assert(txn->nentries_mem == 0);
}

/*
 * 变更的内存大小。   
 */
static Size
ReorderBufferChangeSize(ReorderBufferChange *change)
{
	Size		sz = sizeof(ReorderBufferChange);

	switch (change->action)
	{
			/* 这些分支直接贯穿，彼此足够相似  */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			{
				ReorderBufferTupleBuf *oldtup,
						   *newtup;
				Size		oldlen = 0;
				Size		newlen = 0;

				oldtup = change->data.tp.oldtuple;
				newtup = change->data.tp.newtuple;

				if (oldtup)
				{
					sz += sizeof(HeapTupleData);
					oldlen = oldtup->tuple.t_len;
					sz += oldlen;
				}

				if (newtup)
				{
					sz += sizeof(HeapTupleData);
					newlen = newtup->tuple.t_len;
					sz += newlen;
				}

				break;
			}
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				Size		prefix_size = strlen(change->data.msg.prefix) + 1;

				sz += prefix_size + change->data.msg.message_size +
					sizeof(Size) + sizeof(Size);

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				sz += sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;
				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	snap;

				snap = change->data.snapshot;

				sz += sizeof(SnapshotData) +
					sizeof(TransactionId) * snap->xcnt +
					sizeof(TransactionId) * snap->subxcnt;

				break;
			}
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				sz += sizeof(Oid) * change->data.truncate.nrelids;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			/* ReorderBufferChange 包含了所有重要信息   */
			break;
	}

	return sz;
}


/*
 * 将一批已溢写到磁盘的变更恢复到内存中。    
 */
static Size
ReorderBufferRestoreChanges(ReorderBuffer *rb, ReorderBufferTXN *txn,
							TXNEntryFile *file, XLogSegNo *segno)
{
	Size		restored = 0;
	XLogSegNo	last_segno;
	dlist_mutable_iter cleanup_iter;
	File	   *fd = &file->vfd;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	/* 释放当前条目，为后续腾出内存       */
	dlist_foreach_modify(cleanup_iter, &txn->changes)
	{
		ReorderBufferChange *cleanup =
			dlist_container(ReorderBufferChange, node, cleanup_iter.cur);

		dlist_delete(&cleanup->node);
		ReorderBufferReturnChange(rb, cleanup, true);
	}
	txn->nentries_mem = 0;
	Assert(dlist_is_empty(&txn->changes));

	XLByteToSeg(txn->final_lsn, last_segno, wal_segment_size);

	while (restored < max_changes_in_memory && *segno <= last_segno)
	{
		int			readBytes;
		ReorderBufferDiskChange *ondisk;

		CHECK_FOR_INTERRUPTS();

		if (*fd == -1)
		{
			char		path[MAXPGPATH];

			/* 首次进入  */
			if (*segno == 0)
				XLByteToSeg(txn->first_lsn, *segno, wal_segment_size);

			Assert(*segno != 0 || dlist_is_empty(&txn->changes));

			/*
			 * 无需关心 TLI，仅用于单次运行，因此每个 LSN    
			 * 只会映射到一条特定的 WAL 记录。   
			 */
			ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid,
										*segno);

			*fd = PathNameOpenFile(path, O_RDONLY | PG_BINARY);

			/* 即使失败，重置该偏移量也无妨              */
			file->curOffset = 0;

			if (*fd < 0 && errno == ENOENT)
			{
				*fd = -1;
				(*segno)++;
				continue;
			}
			else if (*fd < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m",
								path)));
		}

		/*
		 * 读取变更的静态定长部分，其含总大小信息。    
		 * 如果无法读到一条记录，我们已到达该文件的    
		 * 末尾。        
		 */
		ReorderBufferSerializeReserve(rb, sizeof(ReorderBufferDiskChange));
		readBytes = FileRead(file->vfd, rb->outbuf,
							 sizeof(ReorderBufferDiskChange),
							 file->curOffset, WAIT_EVENT_REORDER_BUFFER_READ);

		/* eof */
		if (readBytes == 0)
		{
			FileClose(*fd);
			*fd = -1;
			(*segno)++;
			continue;
		}
		else if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) sizeof(ReorderBufferDiskChange))));

		file->curOffset += readBytes;

		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		ReorderBufferSerializeReserve(rb,
									  sizeof(ReorderBufferDiskChange) + ondisk->size);
		ondisk = (ReorderBufferDiskChange *) rb->outbuf;

		readBytes = FileRead(file->vfd,
							 rb->outbuf + sizeof(ReorderBufferDiskChange),
							 ondisk->size - sizeof(ReorderBufferDiskChange),
							 file->curOffset,
							 WAIT_EVENT_REORDER_BUFFER_READ);

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: %m")));
		else if (readBytes != ondisk->size - sizeof(ReorderBufferDiskChange))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from reorderbuffer spill file: read %d instead of %u bytes",
							readBytes,
							(uint32) (ondisk->size - sizeof(ReorderBufferDiskChange)))));

		file->curOffset += readBytes;

		/*
		 * 好，已从磁盘读出完整变更，现在将其恢复   
		 * 为内存格式 
		 */
		ReorderBufferRestoreChange(rb, txn, rb->outbuf);
		restored++;
	}

	return restored;
}

/*
 * 将变更从磁盘格式转换为内存格式，并把它加入该事务的 
 * ->changes 链表。      
 *
 * 注意：data 虽声明为 char*，入口处却指向一个     
 * 经过 maxalign 的缓冲区，因此在该函数的大部分地方 
 * 均可安全假定数据已适当对齐，可直接访问。   
 */
static void
ReorderBufferRestoreChange(ReorderBuffer *rb, ReorderBufferTXN *txn,
						   char *data)
{
	ReorderBufferDiskChange *ondisk;
	ReorderBufferChange *change;

	ondisk = (ReorderBufferDiskChange *) data;

	change = ReorderBufferGetChange(rb);

	/* 复制静态段  */
	memcpy(change, &ondisk->change, sizeof(ReorderBufferChange));

	data += sizeof(ReorderBufferDiskChange);

	/* 逐项恢复其余内容 */
	switch (change->action)
	{
			/* 这些分支直接贯穿，彼此足够相似  */
		case REORDER_BUFFER_CHANGE_INSERT:
		case REORDER_BUFFER_CHANGE_UPDATE:
		case REORDER_BUFFER_CHANGE_DELETE:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_INSERT:
			if (change->data.tp.oldtuple)
			{
				uint32		tuplelen = ((HeapTuple) data)->t_len;

				change->data.tp.oldtuple =
					ReorderBufferGetTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* 恢复 ->tuple  */
				memcpy(&change->data.tp.oldtuple->tuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* 把 t_data 指针重置到新 tuplebuf     */
				change->data.tp.oldtuple->tuple.t_data =
					ReorderBufferTupleBufData(change->data.tp.oldtuple);

				/* 恢复元组数据本身  */
				memcpy(change->data.tp.oldtuple->tuple.t_data, data, tuplelen);
				data += tuplelen;
			}

			if (change->data.tp.newtuple)
			{
				/* 此处 data 可能未适当对齐！      */
				uint32		tuplelen;

				memcpy(&tuplelen, data + offsetof(HeapTupleData, t_len),
					   sizeof(uint32));

				change->data.tp.newtuple =
					ReorderBufferGetTupleBuf(rb, tuplelen - SizeofHeapTupleHeader);

				/* 恢复 ->tuple  */
				memcpy(&change->data.tp.newtuple->tuple, data,
					   sizeof(HeapTupleData));
				data += sizeof(HeapTupleData);

				/* 把 t_data 指针重置到新 tuplebuf     */
				change->data.tp.newtuple->tuple.t_data =
					ReorderBufferTupleBufData(change->data.tp.newtuple);

				/* 恢复元组数据本身  */
				memcpy(change->data.tp.newtuple->tuple.t_data, data, tuplelen);
				data += tuplelen;
			}

			break;
		case REORDER_BUFFER_CHANGE_MESSAGE:
			{
				Size		prefix_size;

				/* 读取前缀*/
				memcpy(&prefix_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.prefix = MemoryContextAlloc(rb->context,
															 prefix_size);
				memcpy(change->data.msg.prefix, data, prefix_size);
				Assert(change->data.msg.prefix[prefix_size - 1] == '\0');
				data += prefix_size;

				/* 读取该消息  */
				memcpy(&change->data.msg.message_size, data, sizeof(Size));
				data += sizeof(Size);
				change->data.msg.message = MemoryContextAlloc(rb->context,
															  change->data.msg.message_size);
				memcpy(change->data.msg.message, data,
					   change->data.msg.message_size);
				data += change->data.msg.message_size;

				break;
			}
		case REORDER_BUFFER_CHANGE_INVALIDATION:
			{
				Size		inval_size = sizeof(SharedInvalidationMessage) *
					change->data.inval.ninvalidations;

				change->data.inval.invalidations =
					MemoryContextAlloc(rb->context, inval_size);

				/* 读取该消息  */
				memcpy(change->data.inval.invalidations, data, inval_size);

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SNAPSHOT:
			{
				Snapshot	oldsnap;
				Snapshot	newsnap;
				Size		size;

				oldsnap = (Snapshot) data;

				size = sizeof(SnapshotData) +
					sizeof(TransactionId) * oldsnap->xcnt +
					sizeof(TransactionId) * (oldsnap->subxcnt + 0);

				change->data.snapshot = MemoryContextAllocZero(rb->context, size);

				newsnap = change->data.snapshot;

				memcpy(newsnap, data, size);
				newsnap->xip = (TransactionId *)
					(((char *) newsnap) + sizeof(SnapshotData));
				newsnap->subxip = newsnap->xip + newsnap->xcnt;
				newsnap->copied = true;
				break;
			}
			/* 基础结构体包含全部数据，轻而易举  */
		case REORDER_BUFFER_CHANGE_TRUNCATE:
			{
				Oid		   *relids;

				relids = ReorderBufferGetRelids(rb,
												change->data.truncate.nrelids);
				memcpy(relids, data, change->data.truncate.nrelids * sizeof(Oid));
				change->data.truncate.relids = relids;

				break;
			}
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_CONFIRM:
		case REORDER_BUFFER_CHANGE_INTERNAL_SPEC_ABORT:
		case REORDER_BUFFER_CHANGE_INTERNAL_COMMAND_ID:
		case REORDER_BUFFER_CHANGE_INTERNAL_TUPLECID:
			break;
	}

	dlist_push_tail(&txn->changes, &change->node);
	txn->nentries_mem++;

	/*
	 * 为恢复的变更更新内存记账。尽管在该分支中         
	 * 恢复变更时并不检查内存限制（该检查仅在解码后    
	 * 初始将变更入队时进行），因为稍后会释放这些       
	 * 变更，                                                          
	 * 那也会更新记账（从计数器扣除大小）。我们不希望  
	 * 在那里出现下溢。         
	 */
	ReorderBufferChangeMemoryUpdate(rb, change, true,
									ReorderBufferChangeSize(change));
}

/*
 * 删除传入事务所有已存储到磁盘的数据。  
 */
static void
ReorderBufferRestoreCleanup(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	XLogSegNo	first;
	XLogSegNo	cur;
	XLogSegNo	last;

	Assert(txn->first_lsn != InvalidXLogRecPtr);
	Assert(txn->final_lsn != InvalidXLogRecPtr);

	XLByteToSeg(txn->first_lsn, first, wal_segment_size);
	XLByteToSeg(txn->final_lsn, last, wal_segment_size);

	/* 遍历所有可能的文件名并删除它们        */
	for (cur = first; cur <= last; cur++)
	{
		char		path[MAXPGPATH];

		ReorderBufferSerializedPath(path, MyReplicationSlot, txn->xid, cur);
		if (unlink(path) != 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", path)));
	}
}

/*
 * 在先前的崩溃或解码会话退出之后，移除槽目录中遗留的 
 * 已序列化重排序缓冲区。    
 */
static void
ReorderBufferCleanupSerializedTXNs(const char *slotname)
{
	DIR		   *spill_dir;
	struct dirent *spill_de;
	struct stat statbuf;
	char		path[MAXPGPATH * 2 + 12];

	sprintf(path, "pg_replslot/%s", slotname);

	/* 这里只处理目录，如果不是我们的就跳过      */
	if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
		return;

	spill_dir = AllocateDir(path);
	while ((spill_de = ReadDirExtended(spill_dir, path, INFO)) != NULL)
	{
		/* 只查看可能属于我们的名称*/
		if (strncmp(spill_de->d_name, "xid", 3) == 0)
		{
			snprintf(path, sizeof(path),
					 "pg_replslot/%s/%s", slotname,
					 spill_de->d_name);

			if (unlink(path) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\" during removal of pg_replslot/%s/xid*: %m",
								path, slotname)));
		}
	}
	FreeDir(spill_dir);
}

/*
 * 给定复制槽、事务 ID 与段号，将对应溢写文件填入     
 * 'path'，这是一个属于调用者的缓冲区，大小至少为          
 * MAXPGPATH。       
 */
static void
ReorderBufferSerializedPath(char *path, ReplicationSlot *slot, TransactionId xid,
							XLogSegNo segno)
{
	XLogRecPtr	recptr;

	XLogSegNoOffsetToRecPtr(segno, 0, wal_segment_size, recptr);

	snprintf(path, MAXPGPATH, "pg_replslot/%s/xid-%u-lsn-%X-%X.spill",
			 NameStr(MyReplicationSlot->data.name),
			 xid, LSN_FORMAT_ARGS(recptr));
}

/*
 * 删除我们在重启/崩溃之后溢写到磁盘的所有数据。它   
 * 会在相应的槽被重用时重新创建。  
 */
void
StartupReorderBuffer(void)
{
	DIR		   *logical_dir;
	struct dirent *logical_de;

	logical_dir = AllocateDir("pg_replslot");
	while ((logical_de = ReadDir(logical_dir, "pg_replslot")) != NULL)
	{
		if (strcmp(logical_de->d_name, ".") == 0 ||
			strcmp(logical_de->d_name, "..") == 0)
			continue;

		/* 若它无法成为槽，则跳过该目录 */
		if (!ReplicationSlotValidateName(logical_de->d_name, DEBUG2))
			continue;

		/*
		 * 好，必是存留的逻辑槽，遍历并删除以       
		 * xid-* 开头的一切         
		 */
		ReorderBufferCleanupSerializedTXNs(logical_de->d_name);
	}
	FreeDir(logical_dir);
}

/* ---------------------------------------
 * TOAST 重组支持      
 * -------------------------------------- 
 */

/*
 * 初始化每元组的 TOAST 重建支持。       
 */
static void
ReorderBufferToastInitHash(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASHCTL		hash_ctl;

	Assert(txn->toast_hash == NULL);

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(ReorderBufferToastEnt);
	hash_ctl.hcxt = rb->context;
	txn->toast_hash = hash_create("ReorderBufferToastHash", 5, &hash_ctl,
								  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * 用于 TOAST 重建的逐个 TOAST 块处理     
 *
 * 追加一个 TOAST 块，以便当拥有该被 TOAST 化 Datum 的元组  
 * 到来时重建它。     
 */
static void
ReorderBufferToastAppendChunk(ReorderBuffer *rb, ReorderBufferTXN *txn,
							  Relation relation, ReorderBufferChange *change)
{
	ReorderBufferToastEnt *ent;
	ReorderBufferTupleBuf *newtup;
	bool		found;
	int32		chunksize;
	bool		isnull;
	Pointer		chunk;
	TupleDesc	desc = RelationGetDescr(relation);
	Oid			chunk_id;
	int32		chunk_seq;

	if (txn->toast_hash == NULL)
		ReorderBufferToastInitHash(rb, txn);

	Assert(IsToastRelation(relation));

	newtup = change->data.tp.newtuple;
	chunk_id = DatumGetObjectId(fastgetattr(&newtup->tuple, 1, desc, &isnull));
	Assert(!isnull);
	chunk_seq = DatumGetInt32(fastgetattr(&newtup->tuple, 2, desc, &isnull));
	Assert(!isnull);

	ent = (ReorderBufferToastEnt *)
		hash_search(txn->toast_hash, &chunk_id, HASH_ENTER, &found);

	if (!found)
	{
		Assert(ent->chunk_id == chunk_id);
		ent->num_chunks = 0;
		ent->last_chunk_seq = 0;
		ent->size = 0;
		ent->reconstructed = NULL;
		dlist_init(&ent->chunks);

		if (chunk_seq != 0)
			elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq 0",
				 chunk_seq, chunk_id);
	}
	else if (found && chunk_seq != ent->last_chunk_seq + 1)
		elog(ERROR, "got sequence entry %d for toast chunk %u instead of seq %d",
			 chunk_seq, chunk_id, ent->last_chunk_seq + 1);

	chunk = DatumGetPointer(fastgetattr(&newtup->tuple, 3, desc, &isnull));
	Assert(!isnull);

	/* 先计算大小，以便稍后能一次性分配正确的大小*/
	if (!VARATT_IS_EXTENDED(chunk))
		chunksize = VARSIZE(chunk) - VARHDRSZ;
	else if (VARATT_IS_SHORT(chunk))
		/* 这可能由 heap_form_tuple 的正常操作引起  */
		chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
	else
		elog(ERROR, "unexpected type of toast chunk");

	ent->size += chunksize;
	ent->last_chunk_seq = chunk_seq;
	ent->num_chunks++;
	dlist_push_tail(&ent->chunks, &change->node);
}

/*
 * 调整 change->newtuple，使其指向内存中的 TOAST 元组而非  
 * 可能已不存在的磁盘 TOAST 元组（如 DROP TABLE、VACUUM）。    
 *
 * 但未变更的 TOAST 元组无法替换，它们仍将指向磁盘上的 
 * TOAST 数据。       
 *
 * 在用解 TOAST 后的元组数据更新既有变更时，我们需要  
 * 更新内存记账信息，因为变更大小会不同。              
 * 否则记账可能失去同步，在意外的时刻触发             
 * 序列化。        
 *
 * 我们只需在重新调整元组前减去该变更的大小，再     
 * 加上新的大小。这让变更看起来像是先被移除然后加   
 * 回，只不过它只是调整记账信息。                  
 *
 * 尤其它无法触发序列化，反正无意义，因为该操作    
 * 发生在提交处理中，恰在把变更交给输出插件的   
 * 时候。                       
 */
static void
ReorderBufferToastReplace(ReorderBuffer *rb, ReorderBufferTXN *txn,
						  Relation relation, ReorderBufferChange *change)
{
	TupleDesc	desc;
	int			natt;
	Datum	   *attrs;
	bool	   *isnull;
	bool	   *free;
	HeapTuple	tmphtup;
	Relation	toast_rel;
	TupleDesc	toast_desc;
	MemoryContext oldcontext;
	ReorderBufferTupleBuf *newtup;
	Size		old_size;

	/* 无 TOAST 元组变更  */
	if (txn->toast_hash == NULL)
		return;

	/*
	 * 我们将要修改变更的大小。为确保记账正确，      
	 * 我们记录当前变更的大小，然后在重新计算变更后    
	 * ，减去记录的大小，最后再重新加上新的大小。  
	 * 我们不会立即减去旧大小，因为若在加上新大小前  
	 * 发生错误，我们会释放这些变更，                         
	 * 而那会更新记账信息（从计数器中扣除大小）。     
	 * 我们不希望在那里出现                                
	 * 下溢。       
	 */
	old_size = ReorderBufferChangeSize(change);

	oldcontext = MemoryContextSwitchTo(rb->context);

	/* INSERT 或 UPDATE 中应该只有 TOAST 元组          */
	Assert(change->data.tp.newtuple);

	desc = RelationGetDescr(relation);

	toast_rel = RelationIdGetRelation(relation->rd_rel->reltoastrelid);
	if (!RelationIsValid(toast_rel))
		elog(ERROR, "could not open toast relation with OID %u (base relation \"%s\")",
			 relation->rd_rel->reltoastrelid, RelationGetRelationName(relation));

	toast_desc = RelationGetDescr(toast_rel);

	/* 是否应该改从栈上分配？      */
	attrs = palloc0(sizeof(Datum) * desc->natts);
	isnull = palloc0(sizeof(bool) * desc->natts);
	free = palloc0(sizeof(bool) * desc->natts);

	newtup = change->data.tp.newtuple;

	heap_deform_tuple(&newtup->tuple, desc, attrs, isnull);

	for (natt = 0; natt < desc->natts; natt++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, natt);
		ReorderBufferToastEnt *ent;
		struct varlena *varlena;

		/* va_rawsize 是原始 Datum 的大小 -- 包括头部在内       */
		struct varatt_external toast_pointer;
		struct varatt_indirect redirect_pointer;
		struct varlena *new_datum = NULL;
		struct varlena *reconstructed;
		dlist_iter	it;
		Size		data_done = 0;

		/* 系统列不会被 TOAST 化  */
		if (attr->attnum < 0)
			continue;

		if (attr->attisdropped)
			continue;

		/* 非 varlena 类型     */
		if (attr->attlen != -1)
			continue;

		/* no data */
		if (isnull[natt])
			continue;

		/* 好，可知持有 TOAST Datum    */
		varlena = (struct varlena *) DatumGetPointer(attrs[natt]);

		/* 若元组非外部存储，则无需任何操作   */
		if (!VARATT_IS_EXTERNAL(varlena))
			continue;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, varlena);

		/*
		 * 检查 TOAST 元组有无变更，如有则替换。 
		 */
		ent = (ReorderBufferToastEnt *)
			hash_search(txn->toast_hash,
						&toast_pointer.va_valueid,
						HASH_FIND,
						NULL);
		if (ent == NULL)
			continue;

		new_datum =
			(struct varlena *) palloc0(INDIRECT_POINTER_SIZE);

		free[natt] = true;

		reconstructed = palloc0(toast_pointer.va_rawsize);

		ent->reconstructed = reconstructed;

		/* 把 TOAST 元组从各部分重新拼接起来  */
		dlist_foreach(it, &ent->chunks)
		{
			bool		isnull;
			ReorderBufferChange *cchange;
			ReorderBufferTupleBuf *ctup;
			Pointer		chunk;

			cchange = dlist_container(ReorderBufferChange, node, it.cur);
			ctup = cchange->data.tp.newtuple;
			chunk = DatumGetPointer(fastgetattr(&ctup->tuple, 3, toast_desc, &isnull));

			Assert(!isnull);
			Assert(!VARATT_IS_EXTERNAL(chunk));
			Assert(!VARATT_IS_SHORT(chunk));

			memcpy(VARDATA(reconstructed) + data_done,
				   VARDATA(chunk),
				   VARSIZE(chunk) - VARHDRSZ);
			data_done += VARSIZE(chunk) - VARHDRSZ;
		}
		Assert(data_done == VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer));

		/* 确保其被标记为已压缩或未压缩*/
		if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			SET_VARSIZE_COMPRESSED(reconstructed, data_done + VARHDRSZ);
		else
			SET_VARSIZE(reconstructed, data_done + VARHDRSZ);

		memset(&redirect_pointer, 0, sizeof(redirect_pointer));
		redirect_pointer.pointer = reconstructed;

		SET_VARTAG_EXTERNAL(new_datum, VARTAG_INDIRECT);
		memcpy(VARDATA_EXTERNAL(new_datum), &redirect_pointer,
			   sizeof(redirect_pointer));

		attrs[natt] = PointerGetDatum(new_datum);
	}

	/*
	 * 在单独的内存中构建元组，再把元组复制回传给   
	 * 输出插件的 tuplebuf。我们无法直接对它调用            
	 * heap_fill_tuple()，因为 attrs[] 会指回当前的内容。        
	 */
	tmphtup = heap_form_tuple(desc, attrs, isnull);
	Assert(newtup->tuple.t_len <= MaxHeapTupleSize);
	Assert(ReorderBufferTupleBufData(newtup) == newtup->tuple.t_data);

	memcpy(newtup->tuple.t_data, tmphtup->t_data, tmphtup->t_len);
	newtup->tuple.t_len = tmphtup->t_len;

	/*
	 * 释放之后不再需要的资源，更持久内容的释放在    
	 * ReorderBufferToastReset() 中。    
	 */
	RelationClose(toast_rel);
	pfree(tmphtup);
	for (natt = 0; natt < desc->natts; natt++)
	{
		if (free[natt])
			pfree(DatumGetPointer(attrs[natt]));
	}
	pfree(attrs);
	pfree(free);
	pfree(isnull);

	MemoryContextSwitchTo(oldcontext);

	/* 减去旧的变更大小     */
	ReorderBufferChangeMemoryUpdate(rb, change, false, old_size);
	/* 现在把变更加回，使用正确的大小  */
	ReorderBufferChangeMemoryUpdate(rb, change, true,
									ReorderBufferChangeSize(change));
}

/*
 * 释放为 TOAST 重建分配的所有资源。        
 */
static void
ReorderBufferToastReset(ReorderBuffer *rb, ReorderBufferTXN *txn)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferToastEnt *ent;

	if (txn->toast_hash == NULL)
		return;

	/* 按顺序遍历哈希表并释放所有内容       */
	hash_seq_init(&hstat, txn->toast_hash);
	while ((ent = (ReorderBufferToastEnt *) hash_seq_search(&hstat)) != NULL)
	{
		dlist_mutable_iter it;

		if (ent->reconstructed != NULL)
			pfree(ent->reconstructed);

		dlist_foreach_modify(it, &ent->chunks)
		{
			ReorderBufferChange *change =
				dlist_container(ReorderBufferChange, node, it.cur);

			dlist_delete(&change->node);
			ReorderBufferReturnChange(rb, change, true);
		}
	}

	hash_destroy(txn->toast_hash);
	txn->toast_hash = NULL;
}


/* ---------------------------------------
 * 逻辑解码的可见性支持         
 *
 *
 * 在使用解码快照时查找实际的 cmin/cmax 值。我们无法 
 * 始终依赖已存储的 cmin/cmax 值，原因有二：        
 *
 * * 元组在单个事务内被多次修改，因而获得一个组合     
 *	 CID。组合 CID 只在单个事务的持续期间内               
 *	 有效。          
 * * 有 cmin 无 cmax 的元组被另一事务删除/更新    
 *	 ，而该事务并非创建它且我们正在查看的事务。     
 *	 由于 cmin、cmax、组合 CID 中只会有一个实际存              
 *	 入堆，因此我们已无法再访问所需的                   
 *	 值。       
 *
 * 为解决这些问题，我们按事务用哈希表存放 (cmin,  
 * cmax) 元组，其键为 (relfilelocator, ctid)，其中保存着     
 * (cmin, cmax) 值。由于完全不理会组合 CID，便顺带   
 * 处理了组合 CID 的问题。既然已有 cmin/cmax 值，     
 * 组合 CID 便无意义了。 
 *
 * 由于这里只关心系统表元组，开销应当           
 * 是可以接受的。          
 *
 * 堆重写会增加复杂度，详情见 rewriteheap.c       
 * 。     
 * ------------------------------------------------------------------------ 
 */

/* 用于按 LSN 高效排序映射文件的结构体  */
typedef struct RewriteMappingFile
{
	XLogRecPtr	lsn;
	char		fname[MAXPGPATH];
} RewriteMappingFile;

#ifdef NOT_USED
static void
DisplayMapping(HTAB *tuplecid_data)
{
	HASH_SEQ_STATUS hstat;
	ReorderBufferTupleCidEnt *ent;

	hash_seq_init(&hstat, tuplecid_data);
	while ((ent = (ReorderBufferTupleCidEnt *) hash_seq_search(&hstat)) != NULL)
	{
		elog(DEBUG3, "mapping: node: %u/%u/%u tid: %u/%u cmin: %u, cmax: %u",
			 ent->key.rlocator.dbOid,
			 ent->key.rlocator.spcOid,
			 ent->key.rlocator.relNumber,
			 ItemPointerGetBlockNumber(&ent->key.tid),
			 ItemPointerGetOffsetNumber(&ent->key.tid),
			 ent->cmin,
			 ent->cmax
			);
	}
}
#endif

/*
 * 把一个映射文件用于 tuplecid_data。 
 *
 * 该映射文件必须已经过验证：a) 已提交 b) 属于我们的  
 * 事务 c) 按 LSN 顺序应用。   
 */
static void
ApplyLogicalMappingFile(HTAB *tuplecid_data, Oid relid, const char *fname)
{
	char		path[MAXPGPATH];
	int			fd;
	int			readBytes;
	LogicalRewriteMappingData map;

	sprintf(path, "pg_logical/mappings/%s", fname);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	while (true)
	{
		ReorderBufferTupleCidKey key;
		ReorderBufferTupleCidEnt *ent;
		ReorderBufferTupleCidEnt *new_ent;
		bool		found;

		/* 注意填充问题       */
		memset(&key, 0, sizeof(ReorderBufferTupleCidKey));

		/* 读取映射直至文件末尾             */
		pgstat_report_wait_start(WAIT_EVENT_REORDER_LOGICAL_MAPPING_READ);
		readBytes = read(fd, &map, sizeof(LogicalRewriteMappingData));
		pgstat_report_wait_end();

		if (readBytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else if (readBytes == 0)	/* EOF */
			break;
		else if (readBytes != sizeof(LogicalRewriteMappingData))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read from file \"%s\": read %d instead of %d bytes",
							path, readBytes,
							(int32) sizeof(LogicalRewriteMappingData))));

		key.rlocator = map.old_locator;
		ItemPointerCopy(&map.old_tid,
						&key.tid);


		ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data, &key, HASH_FIND, NULL);

		/* 没有既有映射，无需更新      */
		if (!ent)
			continue;

		key.rlocator = map.new_locator;
		ItemPointerCopy(&map.new_tid,
						&key.tid);

		new_ent = (ReorderBufferTupleCidEnt *)
			hash_search(tuplecid_data, &key, HASH_ENTER, &found);

		if (found)
		{
			/*
			 * 确保既有映射合理。重写期间有时会更新尚无  
			 * cmax 的旧记录（如 pg_class 自身条目）            
			 * ，这发生在重写期间，因此允许。            
			 */
			Assert(ent->cmin == InvalidCommandId || ent->cmin == new_ent->cmin);
			Assert(ent->cmax == InvalidCommandId || ent->cmax == new_ent->cmax);
		}
		else
		{
			/* 更新映射   */
			new_ent->cmin = ent->cmin;
			new_ent->cmax = ent->cmax;
			new_ent->combocid = ent->combocid;
		}
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", path)));
}


/*
 * 检查 TransactionId 'xid' 是否位于预排序数组 'xip' 中。    
 */
static bool
TransactionIdInArray(TransactionId xid, TransactionId *xip, Size num)
{
	return bsearch(&xid, xip, num,
				   sizeof(TransactionId), xidComparator) != NULL;
}

/*
 * list_sort() 比较器：按 LSN 排序 RewriteMappingFiles。       
 */
static int
file_sort_by_lsn(const ListCell *a_p, const ListCell *b_p)
{
	RewriteMappingFile *a = (RewriteMappingFile *) lfirst(a_p);
	RewriteMappingFile *b = (RewriteMappingFile *) lfirst(b_p);

	if (a->lsn < b->lsn)
		return -1;
	else if (a->lsn > b->lsn)
		return 1;
	return 0;
}

/*
 * 如果存在针对我们的 relid 事务的逻辑重映射文件，则     
 * 应用这些文件。 
 */
static void
UpdateLogicalMappings(HTAB *tuplecid_data, Oid relid, Snapshot snapshot)
{
	DIR		   *mapping_dir;
	struct dirent *mapping_de;
	List	   *files = NIL;
	ListCell   *file;
	Oid			dboid = IsSharedRelation(relid) ? InvalidOid : MyDatabaseId;

	mapping_dir = AllocateDir("pg_logical/mappings");
	while ((mapping_de = ReadDir(mapping_dir, "pg_logical/mappings")) != NULL)
	{
		Oid			f_dboid;
		Oid			f_relid;
		TransactionId f_mapped_xid;
		TransactionId f_create_xid;
		XLogRecPtr	f_lsn;
		uint32		f_hi,
					f_lo;
		RewriteMappingFile *f;

		if (strcmp(mapping_de->d_name, ".") == 0 ||
			strcmp(mapping_de->d_name, "..") == 0)
			continue;

		/* 忽略不属于我们的文件*/
		if (strncmp(mapping_de->d_name, "map-", 4) != 0)
			continue;

		if (sscanf(mapping_de->d_name, LOGICAL_REWRITE_FORMAT,
				   &f_dboid, &f_relid, &f_hi, &f_lo,
				   &f_mapped_xid, &f_create_xid) != 6)
			elog(ERROR, "could not parse filename \"%s\"", mapping_de->d_name);

		f_lsn = ((uint64) f_hi) << 32 | f_lo;

		/* 另一数据库的映射     */
		if (f_dboid != dboid)
			continue;

		/* 另一关系的映射        */
		if (f_relid != relid)
			continue;

		/* 创建事务是否已中止？      */
		if (!TransactionIdDidCommit(f_create_xid))
			continue;

		/* 不属于我们的事务*/
		if (!TransactionIdInArray(f_mapped_xid, snapshot->subxip, snapshot->subxcnt))
			continue;

		/* 相关，加入应用队列   */
		f = palloc(sizeof(RewriteMappingFile));
		f->lsn = f_lsn;
		strcpy(f->fname, mapping_de->d_name);
		files = lappend(files, f);
	}
	FreeDir(mapping_dir);

	/* 排序文件以便按 LSN 顺序应用   */
	list_sort(files, file_sort_by_lsn);

	foreach(file, files)
	{
		RewriteMappingFile *f = (RewriteMappingFile *) lfirst(file);

		elog(DEBUG1, "applying mapping: \"%s\" in %u", f->fname,
			 snapshot->subxip[0]);
		ApplyLogicalMappingFile(tuplecid_data, relid, f->fname);
		pfree(f);
	}
}

/*
 * 在逻辑解码中查找元组的 cmin/cmax，此时无法依赖组合 CID 
 * 。        
 */
bool
ResolveCminCmaxDuringDecoding(HTAB *tuplecid_data,
							  Snapshot snapshot,
							  HeapTuple htup, Buffer buffer,
							  CommandId *cmin, CommandId *cmax)
{
	ReorderBufferTupleCidKey key;
	ReorderBufferTupleCidEnt *ent;
	ForkNumber	forkno;
	BlockNumber blockno;
	bool		updated_mapping = false;

	/*
	 * 如果 tuplecid_data 无效，返回未解析状态。这是因为   
	 * 流式传输进行中的事务时，可能在实际解码前就遇到 
	 * 该 CID 的元组。设想先 INSERT 后 TRUNCATE，当应用      
	 * INSERT 时 TRUNCATE 可能尚未解码。因此在这类情况下， 
	 * 我们假定该 CID 来自未来的命令                      
	 * 。     
	 */
	if (tuplecid_data == NULL)
		return false;

	/* 注意填充问题       */
	memset(&key, 0, sizeof(key));

	Assert(!BufferIsLocal(buffer));

	/*
	 * 从缓冲区获取 relfilelocator，没有更方便的办法      
	 * 来访问它。 
	 */
	BufferGetTag(buffer, &key.rlocator, &forkno, &blockno);

	/* 元组只可能位于主分支中   */
	Assert(forkno == MAIN_FORKNUM);
	Assert(blockno == ItemPointerGetBlockNumber(&htup->t_self));

	ItemPointerCopy(&htup->t_self,
					&key.tid);

restart:
	ent = (ReorderBufferTupleCidEnt *)
		hash_search(tuplecid_data, &key, HASH_FIND, NULL);

	/*
	 * 查找映射失败，需检查表是否曾被重写，若          
	 * 是则应用映射，但只应用一次：在此执行期间    
	 * 不可能有新映射，因为必须持有该关系的           
	 * 锁。   
	 */
	if (ent == NULL && !updated_mapping)
	{
		UpdateLogicalMappings(tuplecid_data, htup->t_tableOid, snapshot);
		/* 再次检查映射，但此次不进行更新  */
		updated_mapping = true;
		goto restart;
	}
	else if (ent == NULL)
		return false;

	if (cmin)
		*cmin = ent->cmin;
	if (cmax)
		*cmax = ent->cmax;
	return true;
}

/*
 * 统计指定事务的失效消息数量。           
 *
 * 返回消息数量，并将 msgs 设置为这些消息的链表         
 * 指针。             
 */
uint32
ReorderBufferGetInvalidations(ReorderBuffer *rb, TransactionId xid,
							  SharedInvalidationMessage **msgs)
{
	ReorderBufferTXN *txn;

	txn = ReorderBufferTXNByXid(rb, xid, false, NULL, InvalidXLogRecPtr,
								false);

	if (txn == NULL)
		return 0;

	*msgs = txn->invalidations;

	return txn->ninvalidations;
}
