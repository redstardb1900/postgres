/*-------------------------------------------------------------------------
 *
 * heapam_visibility.c
 *	  堆中所存元组的可见性规则。
 *
 * 注意：所有 HeapTupleSatisfies 例程都会更新元组的
 * "hint" 状态位，如果我们看到插入或删除事务
 * 现已提交或中止（并且设置提示位是安全的）。
 * 若提示位被改动，就对传入的缓冲区调用 MarkBufferDirtyHint。
 * 调用方不仅必须钉住缓冲区，还必须至少持有
 * 包含该元组的缓冲区上的共享缓冲区内容锁。
 *
 * 注意：使用非 MVCC 快照时，我们必须先检查
 * TransactionIdIsInProgress（它查看 PGPROC 数组），然后再检查
 * TransactionIdDidCommit（它查看 pg_xact）。否则会有竞态
 * 条件：我们可能认定一个刚提交的事务已经崩溃，
 * 因为没有一项检查成功。xact.c 会小心地先把
 * 提交/中止记入 pg_xact，然后再清除 PGPROC 数组里的 MyProc->xid。
 * 这解决了那个问题，但也意味着存在一个窗口，其中
 * TransactionIdIsInProgress 和 TransactionIdDidCommit 都会返回真。
 * 若只检查 TransactionIdDidCommit，就可能把一个元组当成
 * 已提交，而随后的 GetSnapshotData 调用仍认为
 * 发起事务还在进行，于是导致应用层的
 * 不一致。结论是：除少数情形外，所有代码路径都要先检查 TransactionIdIsInProgress，
 * 那些情形是我们正在查看自己主事务的
 * 子事务，因此不可能存在竞态
 * 条件。
 *
 * 这里不能用 TransactionIdDidAbort，因为它不会把崩溃期间
 * 仍在进行的事务当成已中止。我们改用排除法来判定
 * 事务是已中止还是已崩溃。
 *
 * 使用 MVCC 快照时，我们依靠 XidInMVCCSnapshot，而不是
 * TransactionIdIsInProgress，但其余逻辑相同：在判定
 * 该事务已不再进行之前，不要去查 pg_xact。
 *
 *
 * 可见性函数摘要：
 *
 *	 HeapTupleSatisfiesMVCC()
 *		  对给出的快照可见，排除当前命令
 *	 HeapTupleSatisfiesUpdate()
 *		  对即时快照可见，带有调用方提供的命令
 *		  计数器，以及更复杂的结果
 *	 HeapTupleSatisfiesSelf()
 *		  对即时快照和当前命令可见
 *	 HeapTupleSatisfiesDirty()
 *		  与 HeapTupleSatisfiesSelf() 类似，但包含未结束的事务
 *	 HeapTupleSatisfiesVacuum()
 *		  对任何正在运行的事务可见，由 VACUUM 使用
 *	 HeapTupleSatisfiesNonVacuumable()
 *		  HeapTupleSatisfiesVacuum 的快照式 API
 *	 HeapTupleSatisfiesToast()
 *		  除非属于被中断的清理，否则可见，供 TOAST 使用
 *	 HeapTupleSatisfiesAny()
 *		  所有元组均可见
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heapam_visibility.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/subtrans.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/combocid.h"
#include "utils/snapmgr.h"


/*
 * SetHintBits()
 *
 * 在当前合适时，为元组设置提交/中止提示位。
 *
 * 只有当我们知道该事务的提交记录保证会先于
 * 缓冲区被刷到磁盘，或者该表是临时表或不记录 WAL 的表、
 * 崩溃时反正会被抹掉时，设置事务已提交提示位才安全。
 * 这里无法更改页面的 LSN，因为我们可能
 * 只持有缓冲区上的共享锁，因此只有当缓冲区的 LSN
 * 已经新于提交 LSN 时，才能用 LSN 做互锁；
 * 否则就只能先不设置该提示位，直到将来
 * 再次检查这个元组。
 *
 * 把事务标成已中止时，总是可以设置提示位。（heapam.c 中
 * 有些代码依赖这一点！）
 *
 * 另外，若正在清理 HEAP_MOVED_IN 或 HEAP_MOVED_OFF 条目，则
 * 总是可以设置提示位，因为 9.0 之前的 VACUUM FULL 总是使用
 * 同步提交，并且不会移动此前没有提示位的元组。
 * （本子程序并不知道这一点，而是由其调用方来应用。）注意：旧式
 * VACUUM FULL 已经不复存在，但只要我们仍支持从 9.0 之前的数据库做
 * 原地更新，本模块就必须保留对 MOVED_OFF/MOVED_IN 标志位的
 * 支持。
 *
 * 普通提交可能是异步的，因此对它们需要取得该事务的 LSN，
 * 再检查它是否已经刷盘。
 *
 * 调用方应把 xid 作为待检查事务的 XID 传入，若不需要检查则传入
 * InvalidTransactionId。
 */
static inline void
SetHintBits(HeapTupleHeader tuple, Buffer buffer,
			uint16 infomask, TransactionId xid)
{
	if (TransactionIdIsValid(xid))
	{
		/* 注意：此处 xid 必须已知已经提交！ */
		XLogRecPtr	commitLSN = TransactionIdGetCommitLSN(xid);

		if (BufferIsPermanent(buffer) && XLogNeedsFlush(commitLSN) &&
			BufferGetLSNAtomic(buffer) < commitLSN)
		{
			/* 尚未刷盘，且没有 LSN 互锁，因此不设置提示位 */
			return;
		}
	}

	tuple->t_infomask |= infomask;
	MarkBufferDirtyHint(buffer, true);
}

/*
 * HeapTupleSetHintBits --- exported version of SetHintBits()
 *
 * 它必须单独存在，因为 C99 对如何
 * 实现内联函数有一套拙劣的规定。
 */
void
HeapTupleSetHintBits(HeapTupleHeader tuple, Buffer buffer,
					 uint16 infomask, TransactionId xid)
{
	SetHintBits(tuple, buffer, infomask, xid);
}


/*
 * HeapTupleSatisfiesSelf
 *		当且仅当堆元组对"自身"有效时为真。
 *
 * 预期行为见 SNAPSHOT_MVCC 的定义。
 *
 * Note:
 *		假定堆元组有效。
 *
 * "自身"可见需要满足下列条件：
 *
 * ((Xmin == my-transaction &&				该行由当前事务更新，且
 *		(Xmax is null						它未被删除
 *		 [|| Xmax != my-transaction)])			[或由另一事务删除]
 * ||
 *
 * (Xmin is committed &&					该行由已提交事务修改，且
 *		(Xmax is null ||					该行尚未被删除，或
 *			(Xmax != my-transaction &&			该行由另一事务删除
 *			 Xmax is not committed)))			且该事务尚未提交
 */
static bool
HeapTupleSatisfiesSelf(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 供 9.0 之前的二进制升级使用 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，因此必定有 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 执行更新的子事务必定已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else
					return false;
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 执行删除的子事务必定已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
			return false;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必定已中止或已崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}

	/* 到这里，插入事务已经提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;			/* 由其他事务更新 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，因此必定有 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
			return false;
		if (TransactionIdIsInProgress(xmax))
			return true;
		if (TransactionIdDidCommit(xmax))
			return false;
		/* 它必定已中止或已崩溃 */
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
		return true;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必定已中止或已崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	return false;
}

/*
 * HeapTupleSatisfiesAny
 *		占位的 satisfies 例程：任何元组都满足 SnapshotAny。
 */
static bool
HeapTupleSatisfiesAny(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	return true;
}

/*
 * HeapTupleSatisfiesToast
 *		当且仅当堆元组作为 TOAST 行有效时为真。
 *
 * 预期行为见 SNAPSHOT_TOAST 的定义。
 *
 * 这是简化版本，只检查 VACUUM 搬移条件。
 * 它适合 TOAST 使用，因为 TOAST 其实并不想做
 * 自己的时间限定检查；若你能看见包含
 * TOAST 引用的主表行，就应当能看见 TOASTed 值。不过，
 * 清理 TOAST 表与主表相互独立，万一这样的
 * 清理中途失败，我们最好还是做这么多检查。
 *
 * 除了别的含义之外，这意味着不能对 TOAST
 * 表中的行做 UPDATEs。
 */
static bool
HeapTupleSatisfiesToast(HeapTuple htup, Snapshot snapshot,
						Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 供 9.0 之前的二进制升级使用 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}

		/*
		 * 被超级删除取消的推测插入可能留下无效的 Xmin。
		 * 这也适用于推测插入期间创建的
		 * TOAST 元组。
		 */
		else if (!TransactionIdIsValid(HeapTupleHeaderGetXmin(tuple)))
			return false;
	}

	/* 否则假定该元组作为 TOAST 行是有效的。 */
	return true;
}

/*
 * HeapTupleSatisfiesUpdate
 *
 *	本函数返回的结果码比本文件中大多数
 *	函数更细，因为 UPDATE 需要知道的不只是"它是否
 *	可见？"。它还允许使用调用方提供的 CommandId，而不是
 *	依赖 CurrentCommandId。
 *
 *	可能的返回码有：
 *
 *	TM_Invisible：扫描开始时该元组根本不存在，例如它
 *	由更晚的 CommandId 创建。
 *
 *	TM_Ok：元组有效且可见，因此可以被更新。
 *
 *	TM_SelfModified：元组被当前事务更新，且发生在
 *	当前扫描开始之后。
 *
 *	TM_Updated：元组被一个已提交事务更新（包括
 *	元组被移到另一个分区的情形）。
 *
 *	TM_Deleted：元组被一个已提交事务删除。
 *
 *	TM_BeingModified：元组正被一个进行中的事务更新，
 *	且该事务不是当前事务。（注意：这也包括元组被某个 MultiXact
 *	以共享方式锁定的情形，即便该 MultiXact 包含
 *	当前事务。想要区分这种情形的调用方必须
 *	自行检测。）
 */
TM_Result
HeapTupleSatisfiesUpdate(HeapTuple htup, CommandId curcid,
						 Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return TM_Invisible;

		/* 供 9.0 之前的二进制升级使用 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return TM_Invisible;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return TM_Invisible;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return TM_Invisible;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return TM_Invisible;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= curcid)
				return TM_Invisible;	/* 在扫描开始之后被插入 */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return TM_Ok;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			{
				TransactionId xmax;

				xmax = HeapTupleHeaderGetRawXmax(tuple);

				/*
				 * 这里要小心：尽管该元组由我们自己的事务创建，
				 * 它仍可能被其他事务锁住，前提是
				 * 我们更新它的时候，原来的版本正被
				 * 键共享锁锁住。
				 */

				if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
				{
					if (MultiXactIdIsRunning(xmax, true))
						return TM_BeingModified;
					else
						return TM_Ok;
				}

				/*
				 * 若加锁者已经消失，这个 Xmax 里就没有
				 * 值得关心的东西了；否则，把该元组报告为
				 * 已锁定/已更新。
				 */
				if (!TransactionIdIsInProgress(xmax))
					return TM_Ok;
				return TM_BeingModified;
			}

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，因此必定有 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 执行删除的子事务必定已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
				{
					if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple),
											 false))
						return TM_BeingModified;
					return TM_Ok;
				}
				else
				{
					if (HeapTupleHeaderGetCmax(tuple) >= curcid)
						return TM_SelfModified; /* 在扫描开始之后被更新 */
					else
						return TM_Invisible;	/* 在扫描开始之前被更新 */
				}
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 执行删除的子事务必定已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return TM_Ok;
			}

			if (HeapTupleHeaderGetCmax(tuple) >= curcid)
				return TM_SelfModified; /* 在扫描开始之后被更新 */
			else
				return TM_Invisible;	/* 在扫描开始之前被更新 */
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
			return TM_Invisible;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必定已中止或已崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return TM_Invisible;
		}
	}

	/* 到这里，插入事务已经提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return TM_Ok;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return TM_Ok;
		if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
			return TM_Updated;	/* 由其他事务更新 */
		else
			return TM_Deleted;	/* 由其他事务删除 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_LOCKED_UPGRADED(tuple->t_infomask))
			return TM_Ok;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		{
			if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), true))
				return TM_BeingModified;

			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
			return TM_Ok;
		}

		xmax = HeapTupleGetUpdateXid(tuple);
		if (!TransactionIdIsValid(xmax))
		{
			if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
				return TM_BeingModified;
		}

		/* 不是 LOCKED_ONLY，因此必定有 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= curcid)
				return TM_SelfModified; /* 在扫描开始之后被更新 */
			else
				return TM_Invisible;	/* 在扫描开始之前被更新 */
		}

		if (MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
			return TM_BeingModified;

		if (TransactionIdDidCommit(xmax))
		{
			if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
				return TM_Updated;
			else
				return TM_Deleted;
		}

		/*
		 * 到这里，Xmax 中的更新要么已中止，要么已崩溃，但
		 * 其他成员呢？
		 */

		if (!MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
		{
			/*
			 * 已经没有任何成员还活着，哪怕只是一个加锁者，因此可以
			 * 把该 Xmax 标为无效。
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return TM_Ok;
		}
		else
		{
			/* 仍有加锁者在运行 */
			return TM_BeingModified;
		}
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return TM_BeingModified;
		if (HeapTupleHeaderGetCmax(tuple) >= curcid)
			return TM_SelfModified; /* 在扫描开始之后被更新 */
		else
			return TM_Invisible;	/* 在扫描开始之前被更新 */
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
		return TM_BeingModified;

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必定已中止或已崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return TM_Ok;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return TM_Ok;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	if (!ItemPointerEquals(&htup->t_self, &tuple->t_ctid))
		return TM_Updated;		/* 由其他事务更新 */
	else
		return TM_Deleted;		/* 由其他事务删除 */
}

/*
 * HeapTupleSatisfiesDirty
 *		当且仅当堆元组有效、且计入未结束事务的效果时为真。
 *
 * 预期行为见 SNAPSHOT_DIRTY 的定义。
 *
 * 就当前事务以及已提交/已中止事务的效果而言，这本质上与
 * HeapTupleSatisfiesSelf 相同。
 * 不过，我们还会计入其他仍在进行中的事务的效果。
 *
 * 一个特殊做法是：传入的 snapshot 结构被当作
 * 输出参数，用来返回影响了该
 * 元组的并发事务 xid。若元组的 xmin 属于另一个
 * 仍在进行的事务，就把 snapshot->xmin 设为该 xmin；若元组的 xmin
 * 已提交且有效、已提交但已死，或属于我自己的事务，则设为 InvalidTransactionId。
 * snapshot->xmax 与元组的 xmax 同样处理。若该元组是
 * 推测插入的，也就是插入者仍可能收回
 * 这次插入而不中止整个事务，则相应的
 * 令牌也通过 snapshot->speculativeToken 返回。
 */
static bool
HeapTupleSatisfiesDirty(HeapTuple htup, Snapshot snapshot,
						Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	snapshot->xmin = snapshot->xmax = InvalidTransactionId;
	snapshot->speculativeToken = 0;

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 供 9.0 之前的二进制升级使用 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!TransactionIdIsInProgress(xvac))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (TransactionIdIsInProgress(xvac))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，因此必定有 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 执行更新的子事务必定已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else
					return false;
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 执行删除的子事务必定已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			return false;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
		{
			/*
			 * 把推测插入令牌返回给调用方。调用方可以自己操心
			 * xmax，因为那需要一个已最终锁定的行版本，而且
			 * 对该元组的并发更新会与它的
			 * 目的相冲突。
			 */
			if (HeapTupleHeaderIsSpeculative(tuple))
			{
				snapshot->speculativeToken =
					HeapTupleHeaderGetSpeculativeToken(tuple);

				Assert(snapshot->speculativeToken != 0);
			}

			snapshot->xmin = HeapTupleHeaderGetRawXmin(tuple);
			/* XXX 我们不该继续往下查看 xmax 吗？ */
			return true;		/* 正由其他事务插入 */
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必定已中止或已崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}

	/* 到这里，插入事务已经提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;			/* 由其他事务更新 */
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，因此必定有 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
			return false;
		if (TransactionIdIsInProgress(xmax))
		{
			snapshot->xmax = xmax;
			return true;
		}
		if (TransactionIdDidCommit(xmax))
			return false;
		/* 它必定已中止或已崩溃 */
		return true;
	}

	if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			return true;
		return false;
	}

	if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
	{
		if (!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
			snapshot->xmax = HeapTupleHeaderGetRawXmax(tuple);
		return true;
	}

	if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
	{
		/* 它必定已中止或已崩溃 */
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	/* xmax 事务已提交 */

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
					InvalidTransactionId);
		return true;
	}

	SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
				HeapTupleHeaderGetRawXmax(tuple));
	return false;				/* 由其他事务更新 */
}

/*
 * HeapTupleSatisfiesMVCC
 *		当且仅当堆元组对给定的 MVCC 快照有效时为真。
 *
 * 预期行为见 SNAPSHOT_MVCC 的定义。
 *
 * 注意：若按我们的快照，插入或删除事务仍在运行，
 * 这里就不会更新元组的状态提示位，
 * 即便它实际上此刻已经提交或中止。这是有意为之。
 * 去检查真实的事务状态需要访问流量很高的
 * 共享数据结构，造成我们宁可不要的争用，而且这
 * 反正也不会改变可见性检查的结果。提示位
 * 将由第一个快照已经新到能够看见
 * 插入或删除事务已经结束的访问者来更新。在此期间，让
 * 提示位保持未设置的代价大体是：每次 HeapTupleSatisfiesMVCC
 * 调用除了要执行 XidInMVCCSnapshot 之外，还得再执行 TransactionIdIsCurrentTransactionId
 * （不过后者本来也要执行）。在旧的
 * 写法里，我们试图尽快设置提示位，于是每次调用改为执行
 * TransactionIdIsInProgress——只要插入或删除事务仍在运行，
 * 这就毫无益处——既增加了周期开销，
 * 也加重了对 ProcArrayLock 的争用。
 */
static bool
HeapTupleSatisfiesMVCC(HeapTuple htup, Snapshot snapshot,
					   Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return false;

		/* 供 9.0 之前的二进制升级使用 */
		if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return false;
			if (!XidInMVCCSnapshot(xvac, snapshot))
			{
				if (TransactionIdDidCommit(xvac))
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			}
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (!TransactionIdIsCurrentTransactionId(xvac))
			{
				if (XidInMVCCSnapshot(xvac, snapshot))
					return false;
				if (TransactionIdDidCommit(xvac))
					SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
								InvalidTransactionId);
				else
				{
					SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
								InvalidTransactionId);
					return false;
				}
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (HeapTupleHeaderGetCmin(tuple) >= snapshot->curcid)
				return false;	/* 在扫描开始之后被插入 */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return true;

			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))	/* 不是删除者 */
				return true;

			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				TransactionId xmax;

				xmax = HeapTupleGetUpdateXid(tuple);

				/* 不是 LOCKED_ONLY，因此必定有 xmax */
				Assert(TransactionIdIsValid(xmax));

				/* 执行更新的子事务必定已中止 */
				if (!TransactionIdIsCurrentTransactionId(xmax))
					return true;
				else if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
					return true;	/* 在扫描开始之后被更新 */
				else
					return false;	/* 在扫描开始之前被更新 */
			}

			if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
			{
				/* 执行删除的子事务必定已中止 */
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
				return true;
			}

			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 在扫描开始之后被删除 */
			else
				return false;	/* 在扫描开始之前被删除 */
		}
		else if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmin(tuple), snapshot))
			return false;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/* 它必定已中止或已崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return false;
		}
	}
	else
	{
		/* xmin 已提交，但按我们的快照未必如此 */
		if (!HeapTupleHeaderXminFrozen(tuple) &&
			XidInMVCCSnapshot(HeapTupleHeaderGetRawXmin(tuple), snapshot))
			return false;		/* 视为仍在进行中 */
	}

	/* 到这里，插入事务已经提交 */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效或已中止 */
		return true;

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return true;

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax;

		/* 上面已经检查过 */
		Assert(!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		xmax = HeapTupleGetUpdateXid(tuple);

		/* 不是 LOCKED_ONLY，因此必定有 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsCurrentTransactionId(xmax))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 在扫描开始之后被删除 */
			else
				return false;	/* 在扫描开始之前被删除 */
		}
		if (XidInMVCCSnapshot(xmax, snapshot))
			return true;
		if (TransactionIdDidCommit(xmax))
			return false;		/* 更新事务已提交 */
		/* 它必定已中止或已崩溃 */
		return true;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmax(tuple)))
		{
			if (HeapTupleHeaderGetCmax(tuple) >= snapshot->curcid)
				return true;	/* 在扫描开始之后被删除 */
			else
				return false;	/* 在扫描开始之前被删除 */
		}

		if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmax(tuple), snapshot))
			return true;

		if (!TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
		{
			/* 它必定已中止或已崩溃 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return true;
		}

		/* xmax 事务已提交 */
		SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
					HeapTupleHeaderGetRawXmax(tuple));
	}
	else
	{
		/* xmax 已提交，但按我们的快照未必如此 */
		if (XidInMVCCSnapshot(HeapTupleHeaderGetRawXmax(tuple), snapshot))
			return true;		/* 视为仍在进行中 */
	}

	/* xmax 事务已提交 */

	return false;
}


/*
 * HeapTupleSatisfiesVacuum
 *
 *	为 VACUUM 判定元组状态。这里我们
 *	主要想知道元组是否可能对任一
 *	正在运行的事务可见。若是，VACUUM 还不能移除它。
 *
 * OldestXmin 是一个截止 XID（取自
 * GetOldestNonRemovableTransactionId()）。由 XIDs >=
 * OldestXmin 删除的元组被视为"最近死亡"；它们对某些
 * 未结束的事务可能仍然可见，因此即使看到删除
 * 事务已经提交，也不能移除它们。
 */
HTSV_Result
HeapTupleSatisfiesVacuum(HeapTuple htup, TransactionId OldestXmin,
						 Buffer buffer)
{
	TransactionId dead_after = InvalidTransactionId;
	HTSV_Result res;

	res = HeapTupleSatisfiesVacuumHorizon(htup, buffer, &dead_after);

	if (res == HEAPTUPLE_RECENTLY_DEAD)
	{
		Assert(TransactionIdIsValid(dead_after));

		if (TransactionIdPrecedes(dead_after, OldestXmin))
			res = HEAPTUPLE_DEAD;
	}
	else
		Assert(!TransactionIdIsValid(dead_after));

	return res;
}

/*
 * HeapTupleSatisfiesVacuum 及同类例程的主力实现。
 *
 * 与 HeapTupleSatisfiesVacuum 相对：本例程在遇到一个
 * 仍可能对某个后端可见的元组时，会把那个 xid 存起来，
 * 该 xid 需要与视界比较，存放位置是 *dead_after，并返回
 * HEAPTUPLE_RECENTLY_DEAD。随后调用方可以自己去做与
 * 视界的比较。例如在对照不同视界时，这样做很有用。
 *
 * 注意：这里仍可能返回 HEAPTUPLE_DEAD，例如当插入
 * 事务已中止时。
 */
HTSV_Result
HeapTupleSatisfiesVacuumHorizon(HeapTuple htup, Buffer buffer, TransactionId *dead_after)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);
	Assert(dead_after != NULL);

	*dead_after = InvalidTransactionId;

	/*
	 * 插入事务提交了吗？
	 *
	 * 若插入事务已中止，则该元组对任何其他事务都从未可见，
	 * 因此可以立刻删除它。
	 */
	if (!HeapTupleHeaderXminCommitted(tuple))
	{
		if (HeapTupleHeaderXminInvalid(tuple))
			return HEAPTUPLE_DEAD;
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_OFF)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
			{
				SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
							InvalidTransactionId);
				return HEAPTUPLE_DEAD;
			}
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						InvalidTransactionId);
		}
		/* 供 9.0 之前的二进制升级使用 */
		else if (tuple->t_infomask & HEAP_MOVED_IN)
		{
			TransactionId xvac = HeapTupleHeaderGetXvac(tuple);

			if (TransactionIdIsCurrentTransactionId(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdIsInProgress(xvac))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			if (TransactionIdDidCommit(xvac))
				SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
							InvalidTransactionId);
			else
			{
				SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
							InvalidTransactionId);
				return HEAPTUPLE_DEAD;
			}
		}
		else if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple)))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid 无效 */
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			/* 仅仅被锁住？为了性能，先做只看 infomask 的检查 */
			if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask) ||
				HeapTupleHeaderIsOnlyLocked(tuple))
				return HEAPTUPLE_INSERT_IN_PROGRESS;
			/* 由同一事务先插入再删除 */
			if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetUpdateXid(tuple)))
				return HEAPTUPLE_DELETE_IN_PROGRESS;
			/* 执行删除的子事务必定已中止 */
			return HEAPTUPLE_INSERT_IN_PROGRESS;
		}
		else if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmin(tuple)))
		{
			/*
			 * 这里本可以区分进行中的 INSERT 与 DELETE，
			 * 办法是查看 xmax，但这对大多数调用方似乎并无好处，
			 * 对其中一些甚至有害。我们更希望
			 * 调用方去查看或等待 xmin，而不是 xmax。返回
			 * INSERT_IN_PROGRESS 总是正确的，因为那正是
			 * 从其他后端的视角所看到的正在发生的事。
			 */
			return HEAPTUPLE_INSERT_IN_PROGRESS;
		}
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmin(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMIN_COMMITTED,
						HeapTupleHeaderGetRawXmin(tuple));
		else
		{
			/*
			 * 不在进行中，也未提交，因此要么已中止，要么已崩溃
			 */
			SetHintBits(tuple, buffer, HEAP_XMIN_INVALID,
						InvalidTransactionId);
			return HEAPTUPLE_DEAD;
		}

		/*
		 * 至此 xmin 已知已提交，但我们可能还没能
		 * 设置提示位；因此不能再 Assert 它
		 * 已经置位。
		 */
	}

	/*
	 * 好，插入者已提交，所以它在某个时刻曾经是好的。那么
	 * 删除事务呢？
	 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return HEAPTUPLE_LIVE;

	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
	{
		/*
		 * 所谓删除事务其实只是锁住了它，因此元组在任何
		 * 情况下都是活的。不过，一旦该事务消失，应确保 XMAX_COMMITTED 或
		 * XMAX_INVALID 之一被置位，以降低今后事务
		 * 检查该元组的代价。
		 */
		if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
		{
			if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
			{
				/*
				 * 若它是 pg_upgrade 之前的元组，该多事务不可能
				 * 仍在运行；否则就必须检查。
				 */
				if (!HEAP_LOCKED_UPGRADED(tuple->t_infomask) &&
					MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple),
										 true))
					return HEAPTUPLE_LIVE;
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
			}
			else
			{
				if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
					return HEAPTUPLE_LIVE;
				SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
							InvalidTransactionId);
			}
		}

		/*
		 * 我们并不真正在意 xmax 是提交、中止还是崩溃。我们
		 * 知道 xmax 确实锁过该元组，但它没有、也永远不会
		 * 真正更新它。
		 */

		return HEAPTUPLE_LIVE;
	}

	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		TransactionId xmax = HeapTupleGetUpdateXid(tuple);

		/* 上面已经检查过 */
		Assert(!HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask));

		/* 不是 LOCKED_ONLY，因此必定有 xmax */
		Assert(TransactionIdIsValid(xmax));

		if (TransactionIdIsInProgress(xmax))
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		else if (TransactionIdDidCommit(xmax))
		{
			/*
			 * 由于还有加锁者，该多事务可能仍在运行。无论
			 * 是否如此，只要低于 xid 视界，都要允许剪枝——
			 * 否则可能出现这样的元组：更新者因视界
			 * 必须被移除，却没有被剪掉。剪掉
			 * 该元组没有问题，因为任何残留的
			 * 加锁者也会出现在更新的元组版本中。
			 */
			*dead_after = xmax;
			return HEAPTUPLE_RECENTLY_DEAD;
		}
		else if (!MultiXactIdIsRunning(HeapTupleHeaderGetRawXmax(tuple), false))
		{
			/*
			 * 不在进行中，也未提交，因此要么已中止，要么已崩溃。
			 * 把该 Xmax 标为无效。
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID, InvalidTransactionId);
		}

		return HEAPTUPLE_LIVE;
	}

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsInProgress(HeapTupleHeaderGetRawXmax(tuple)))
			return HEAPTUPLE_DELETE_IN_PROGRESS;
		else if (TransactionIdDidCommit(HeapTupleHeaderGetRawXmax(tuple)))
			SetHintBits(tuple, buffer, HEAP_XMAX_COMMITTED,
						HeapTupleHeaderGetRawXmax(tuple));
		else
		{
			/*
			 * 不在进行中，也未提交，因此要么已中止，要么已崩溃
			 */
			SetHintBits(tuple, buffer, HEAP_XMAX_INVALID,
						InvalidTransactionId);
			return HEAPTUPLE_LIVE;
		}

		/*
		 * 至此 xmax 已知已提交，但我们可能还没能
		 * 设置提示位；因此不能再 Assert 它
		 * 已经置位。
		 */
	}

	/*
	 * 删除者已提交，允许调用方检查它是否足够新近，以至于
	 * 某些未结束的事务仍可能看见该元组。
	 */
	*dead_after = HeapTupleHeaderGetRawXmax(tuple);
	return HEAPTUPLE_RECENTLY_DEAD;
}


/*
 * HeapTupleSatisfiesNonVacuumable
 *
 *	若元组可能对某个事务可见则为真；若它对
 *	所有人都确定已死，即可被清理，则为假。
 *
 *	预期行为见 SNAPSHOT_NON_VACUUMABLE 的定义。
 *
 *	这是 HeapTupleSatisfiesVacuum 的接口，可通过
 *	HeapTupleSatisfiesSnapshot 调用，因此能借助 Snapshot 使用。
 *	snapshot->vistest 必须已按要使用的视界设置好。
 */
static bool
HeapTupleSatisfiesNonVacuumable(HeapTuple htup, Snapshot snapshot,
								Buffer buffer)
{
	TransactionId dead_after = InvalidTransactionId;
	HTSV_Result res;

	res = HeapTupleSatisfiesVacuumHorizon(htup, buffer, &dead_after);

	if (res == HEAPTUPLE_RECENTLY_DEAD)
	{
		Assert(TransactionIdIsValid(dead_after));

		if (GlobalVisTestIsRemovableXid(snapshot->vistest, dead_after))
			res = HEAPTUPLE_DEAD;
	}
	else
		Assert(!TransactionIdIsValid(dead_after));

	return res != HEAPTUPLE_DEAD;
}


/*
 * HeapTupleIsSurelyDead
 *
 *	低成本地判断元组是否对所有旁观者都确定已死。
 *	有时用它代替 HeapTupleSatisfiesVacuum，时机是该
 *	元组刚被另一个可见性例程测试过（通常是
 *	HeapTupleSatisfiesMVCC），因此凡是能够设置的提示位
 *	都应当已经设好。我们假定若提示位都没设，则 xmin
 *	或 xmax 事务仍在运行。所以它比
 *	HeapTupleSatisfiesVacuum 更快，因为既不查阅 procarray，也不查阅 CLOG。
 *	拿不准时可以返回假，但只有当
 *	元组可以被移除时，才必须返回真。
 */
bool
HeapTupleIsSurelyDead(HeapTuple htup, GlobalVisState *vistest)
{
	HeapTupleHeader tuple = htup->t_data;

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	/*
	 * 若插入事务被标为无效，则它已中止，
	 * 元组确定已死。若它既未标为已提交也未标为
	 * 无效，则我们假定它仍活着（因为前提是
	 * 所有相关提示位片刻之前刚刚设好）。
	 */
	if (!HeapTupleHeaderXminCommitted(tuple))
		return HeapTupleHeaderXminInvalid(tuple);

	/*
	 * 若插入事务已提交，但任何删除事务
	 * 都已中止，则元组仍然活着。
	 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return false;

	/*
	 * 若 XMAX 只是一把锁，元组仍然活着。
	 */
	if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return false;

	/*
	 * 若 Xmax 是 MultiXact，它可能已死也可能还活着，但不检查
	 * pg_multixact 就无法知道。
	 */
	if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
		return false;

	/* 若尚不能确定删除者已提交，就假定它仍在运行。 */
	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
		return false;

	/* 删除者已提交，因此若该 XID 足够老，元组就是死的。 */
	return GlobalVisTestIsRemovableXid(vistest,
									   HeapTupleHeaderGetRawXmax(tuple));
}

/*
 * 该元组是否真的只被锁定？也就是说，它没有被更新？
 *
 * 若加锁者不是 multi，只检查 infomask 位很容易；但
 * 否则就需要验证执行更新的事务没有中止。
 *
 * 这个函数放在这里，是因为它遵循的可见性规则与
 * 本文件开头所写的相同。
 */
bool
HeapTupleHeaderIsOnlyLocked(HeapTupleHeader tuple)
{
	TransactionId xmax;

	/* 若没有有效的 Xmax，显然也就没有更新 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return true;

	if (tuple->t_infomask & HEAP_XMAX_LOCK_ONLY)
		return true;

	/* 无效的 xmax 表示没有更新 */
	if (!TransactionIdIsValid(HeapTupleHeaderGetRawXmax(tuple)))
		return true;

	/*
	 * 若未设置 HEAP_XMAX_LOCK_ONLY 且不是 multi，则这必定
	 * 已经被更新过
	 */
	if (!(tuple->t_infomask & HEAP_XMAX_IS_MULTI))
		return false;

	/* ...但若它是 multi，则执行更新的 Xid 也许已经中止。 */
	xmax = HeapTupleGetUpdateXid(tuple);

	/* 不是 LOCKED_ONLY，因此必定有 xmax */
	Assert(TransactionIdIsValid(xmax));

	if (TransactionIdIsCurrentTransactionId(xmax))
		return false;
	if (TransactionIdIsInProgress(xmax))
		return false;
	if (TransactionIdDidCommit(xmax))
		return false;

	/*
	 * 不是当前事务，不在进行中，也未提交——必定已中止或
	 * 已崩溃
	 */
	return true;
}

/*
 * 检查事务 id 'xid' 是否位于预先排好序的数组 'xip' 中。
 */
static bool
TransactionIdInArray(TransactionId xid, TransactionId *xip, Size num)
{
	return num > 0 &&
		bsearch(&xid, xip, num, sizeof(TransactionId), xidComparator) != NULL;
}

/*
 * 本函数所遵守的语义，见 HeapTupleSatisfiesMVCC 的
 * 注释。
 *
 * 只适用于目录表中的元组！
 *
 * 目前不需要支持 HEAP_MOVED_(IN|OFF)，因为我们只支持
 * 读取不可能由更老版本创建的目录页。
 *
 * 这里不设置任何提示位，因为那样做似乎没什么好处，
 * 正常访问应当已经把它们设好了，而且这样做似乎过于
 * 危险：在 timetravel 期间设置提示位的语义，比只处理
 * "当前"时更加复杂。
 */
static bool
HeapTupleSatisfiesHistoricMVCC(HeapTuple htup, Snapshot snapshot,
							   Buffer buffer)
{
	HeapTupleHeader tuple = htup->t_data;
	TransactionId xmin = HeapTupleHeaderGetXmin(tuple);
	TransactionId xmax = HeapTupleHeaderGetRawXmax(tuple);

	Assert(ItemPointerIsValid(&htup->t_self));
	Assert(htup->t_tableOid != InvalidOid);

	/* 插入事务已中止 */
	if (HeapTupleHeaderXminInvalid(tuple))
	{
		Assert(!TransactionIdDidCommit(xmin));
		return false;
	}
	/* 检查它是否是我们的 txid 之一，顶层事务也在其中 */
	else if (TransactionIdInArray(xmin, snapshot->subxip, snapshot->subxcnt))
	{
		bool		resolved;
		CommandId	cmin = HeapTupleHeaderGetRawCommandId(tuple);
		CommandId	cmax = InvalidCommandId;

		/*
		 * 另一个事务可能已经（试图）删除该元组，或者
		 * cmin/cmax 被存在了 combo CID 里。因此需要在外部
		 * 查找实际的值。
		 */
		resolved = ResolveCminCmaxDuringDecoding(HistoricSnapshotGetTupleCids(), snapshot,
												 htup, buffer,
												 &cmin, &cmax);

		/*
		 * 若还没把 combo CID 解析成 cmin/cmax，那就表示我们
		 * 尚未解码该 combo CID。这意味着 cmin
		 * 肯定处于将来，因此现在还不应当看见这个
		 * 元组。
		 *
		 * XXX 这只适用于对进行中事务的解码。在
		 * 常规逻辑解码中，我们只在提交时执行这段代码，
		 * 那时应该已经见过所有相关的 combo CIDs。所以
		 * 理想情况下这里应该报错，但实践中这
		 * 不会发生。若对此过于担心，可以在
		 * ResolveCminCmaxDuringDecoding 里加一条 elog。
		 *
		 * XXX 对于流式情形，可以跟踪已分配的最大 combo CID，
		 * 并据此报错（当无法解析那个已观测最大值
		 * 以下的 combo CID 时）。
		 */
		if (!resolved)
			return false;

		Assert(cmin != InvalidCommandId);

		if (cmin >= snapshot->curcid)
			return false;		/* 在扫描开始之后被插入 */
		/* fall through */
	}
	/* 在我们的 xmin 视界之前已提交。做一次常规可见性检查。 */
	else if (TransactionIdPrecedes(xmin, snapshot->xmin))
	{
		Assert(!(HeapTupleHeaderXminCommitted(tuple) &&
				 !TransactionIdDidCommit(xmin)));

		/* 先检查提示位，然后再查阅 clog */
		if (!HeapTupleHeaderXminCommitted(tuple) &&
			!TransactionIdDidCommit(xmin))
			return false;
		/* fall through */
	}
	/* 超出我们的 xmax 视界，即不可见 */
	else if (TransactionIdFollowsOrEquals(xmin, snapshot->xmax))
	{
		return false;
	}
	/* 检查它是否是 [xmin, xmax) 内已提交的事务 */
	else if (TransactionIdInArray(xmin, snapshot->xip, snapshot->xcnt))
	{
		/* fall through */
	}

	/*
	 * 以上都不是，即落在 [xmin, xmax) 之中但尚未提交。也就是
	 * 不可见。
	 */
	else
	{
		return false;
	}

	/* 至此已知 xmin 可见，继续检查 xmax */

	/* xid 无效或已中止 */
	if (tuple->t_infomask & HEAP_XMAX_INVALID)
		return true;
	/* 被锁定的元组总是可见 */
	else if (HEAP_XMAX_IS_LOCKED_ONLY(tuple->t_infomask))
		return true;

	/*
	 * 若正在查看用户表，或者有人对系统表做了
	 * SELECT ... FOR SHARE/UPDATE，这里就能看见 multi。
	 */
	else if (tuple->t_infomask & HEAP_XMAX_IS_MULTI)
	{
		xmax = HeapTupleGetUpdateXid(tuple);
	}

	/* 检查它是否是我们的 txid 之一，顶层事务也在其中 */
	if (TransactionIdInArray(xmax, snapshot->subxip, snapshot->subxcnt))
	{
		bool		resolved;
		CommandId	cmin;
		CommandId	cmax = HeapTupleHeaderGetRawCommandId(tuple);

		/* 查找实际的 cmin/cmax 值 */
		resolved = ResolveCminCmaxDuringDecoding(HistoricSnapshotGetTupleCids(), snapshot,
												 htup, buffer,
												 &cmin, &cmax);

		/*
		 * 若还没把 combo CID 解析成 cmin/cmax，那就表示我们
		 * 尚未解码该 combo CID。这意味着 cmax
		 * 肯定处于将来，因此我们现在仍应当看见这个
		 * 元组。
		 *
		 * XXX 这只适用于对进行中事务的解码。在
		 * 常规逻辑解码中，我们只在提交时执行这段代码，
		 * 那时应该已经见过所有相关的 combo CIDs。所以
		 * 理想情况下这里应该报错，但实践中这
		 * 不会发生。若对此过于担心，可以在
		 * ResolveCminCmaxDuringDecoding 里加一条 elog。
		 *
		 * XXX 对于流式情形，可以跟踪已分配的最大 combo CID，
		 * 并据此报错（当无法解析那个已观测最大值
		 * 以下的 combo CID 时）。
		 */
		if (!resolved || cmax == InvalidCommandId)
			return true;

		if (cmax >= snapshot->curcid)
			return true;		/* 在扫描开始之后被删除 */
		else
			return false;		/* 在扫描开始之前被删除 */
	}
	/* 低于 xmin 视界时，常规事务状态有效 */
	else if (TransactionIdPrecedes(xmax, snapshot->xmin))
	{
		Assert(!(tuple->t_infomask & HEAP_XMAX_COMMITTED &&
				 !TransactionIdDidCommit(xmax)));

		/* 先检查提示位 */
		if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
			return false;

		/* 检查 clog */
		return !TransactionIdDidCommit(xmax);
	}
	/* 高于 xmax 视界，我们不可能看见删除事务 */
	else if (TransactionIdFollowsOrEquals(xmax, snapshot->xmax))
		return true;
	/* xmax 位于 [xmin, xmax) 之中，检查已知已提交数组 */
	else if (TransactionIdInArray(xmax, snapshot->xip, snapshot->xcnt))
		return false;
	/* xmax 位于 [xmin, xmax) 之中，但已知尚未提交 */
	else
		return true;
}

/*
 * HeapTupleSatisfiesVisibility
 *		当且仅当堆元组满足时间限定条件时为真。
 *
 * 说明：
 *	假定堆元组有效，且缓冲区至少被共享锁定。
 *
 *	作为副作用，可能更新 HeapTuple 的 t_infomask 中的提示位；
 *	若如此，所指的缓冲区会被标为脏。
 */
bool
HeapTupleSatisfiesVisibility(HeapTuple htup, Snapshot snapshot, Buffer buffer)
{
	switch (snapshot->snapshot_type)
	{
		case SNAPSHOT_MVCC:
			return HeapTupleSatisfiesMVCC(htup, snapshot, buffer);
		case SNAPSHOT_SELF:
			return HeapTupleSatisfiesSelf(htup, snapshot, buffer);
		case SNAPSHOT_ANY:
			return HeapTupleSatisfiesAny(htup, snapshot, buffer);
		case SNAPSHOT_TOAST:
			return HeapTupleSatisfiesToast(htup, snapshot, buffer);
		case SNAPSHOT_DIRTY:
			return HeapTupleSatisfiesDirty(htup, snapshot, buffer);
		case SNAPSHOT_HISTORIC_MVCC:
			return HeapTupleSatisfiesHistoricMVCC(htup, snapshot, buffer);
		case SNAPSHOT_NON_VACUUMABLE:
			return HeapTupleSatisfiesNonVacuumable(htup, snapshot, buffer);
	}

	return false;				/* 免得编译器报警 */
}
