/*-------------------------------------------------------------------------
 * xid8funcs.c
 *
 *	将内部事务 ID 导出给用户层。
 *
 * 注意：只有顶层事务 ID 会暴露给用户会话。
 * 这一点很重要，因为 xid8 的存续期常常超出全局
 * xmin 水平线，甚至可能被传到其他机器上，故无法
 * 依靠子事务 ID 与其父事务建立关联，
 * 如用 SubTransGetTopmostTransaction() 等函数。
 *
 * 这些函数用于支持 txid_XXX 函数以及较新的
 * pg_current_xact_id、pg_current_snapshot 及相关 fmgr 函数，因为
 * 二者唯一的区别在于对外暴露的是 xid8 还是 int8 值，
 * txid_XXX 变体最终应被移除。
 *
 *
 *	Copyright (c) 2003-2023, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *	64-bit txids: Marko Kreen, Skype Technologies
 *
 *	src/backend/utils/adt/xid8funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/xid8.h"


/*
 * 若定义此宏，当快照中 xid8 数量超过指定值时，
 * 改用 bsearch() 函数进行搜索。
 */
#define USE_BSEARCH_IF_NXIP_GREATER 30


/*
 * 包含完整事务标识符的快照。
 */
typedef struct
{
	/*
	 * 4 字节长度头，不应直接改动。
	 *
	 * 显式内嵌没问题，反正我们总要保证正确对齐。
	 */
	int32		__varsz;

	uint32		nxip;			/* xip 数组中 fxid 的数量 */
	FullTransactionId xmin;
	FullTransactionId xmax;
	/* 进行中 fxid，xmin <= xip[i] < xmax: */
	FullTransactionId xip[FLEXIBLE_ARRAY_MEMBER];
} pg_snapshot;

#define PG_SNAPSHOT_SIZE(nxip) \
	(offsetof(pg_snapshot, xip) + sizeof(FullTransactionId) * (nxip))
#define PG_SNAPSHOT_MAX_NXIP \
	((MaxAllocSize - offsetof(pg_snapshot, xip)) / sizeof(FullTransactionId))

/*
 * procarray 的编译期限定（MAX_BACKENDS 个进程加
 * MAX_BACKENDS 个已准备事务）保证了 nxip 不会过大。
 */
StaticAssertDecl(MAX_BACKENDS * 2 <= PG_SNAPSHOT_MAX_NXIP,
				 "possible overflow in pg_current_snapshot()");


/*
 * 辅助函数：从 64 位 xid 获取 TransactionId，并检测回绕。
 *
 * 若 xid 位于未来则报 ERROR；否则，若事务仍足够新、
 * 能判断其是否已提交，则返回真，否则返回假。
 * 若 *extracted_xid 非 NULL，则将其设为
 * 事务 ID 的低 32 位，即实际 XID（不含纪元）。
 *
 *
 * 调用方必须持有 XactTruncationLock，因其处理的是任意
 * XID，且在完成与这些 XID 相关的 clog 查找之前，
 * 须继续持有。
 */
static bool
TransactionIdInRecentPast(FullTransactionId fxid, TransactionId *extracted_xid)
{
	TransactionId xid = XidFromFullTransactionId(fxid);
	uint32		now_epoch;
	TransactionId now_epoch_next_xid;
	FullTransactionId now_fullxid;
	TransactionId oldest_xid;
	FullTransactionId oldest_fxid;

	now_fullxid = ReadNextFullTransactionId();
	now_epoch_next_xid = XidFromFullTransactionId(now_fullxid);
	now_epoch = EpochFromFullTransactionId(now_fullxid);

	if (extracted_xid != NULL)
		*extracted_xid = xid;

	if (!TransactionIdIsValid(xid))
		return false;

	/* 对于非普通事务 ID，可以忽略纪元。 */
	if (!TransactionIdIsNormal(xid))
		return true;

	/* 若事务 ID 位于未来，则抛出错误。 */
	if (!FullTransactionIdPrecedes(fxid, now_fullxid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("transaction ID %llu is in the future",
						(unsigned long long) U64FromFullTransactionId(fxid))));

	/*
	 * ShmemVariableCache->oldestClogXid 由 XactTruncationLock 保护，
	 * 但这里并不获取该锁，而是要求调用方来获取，
	 * 因为调用方随后大概率要查找
	 * 返回的 XID。若在本函数内获取并释放该锁，
	 * CLOG 截断可能在调用方用完该 XID 之前发生。
	 */
	Assert(LWLockHeldByMe(XactTruncationLock));

	/*
	 * 若 fxid 不早于 ShmemVariableCache->oldestClogXid，则
	 * 相应 CLOG 条目必然仍然存在。将
	 * ShmemVariableCache->oldestClogXid 转成完整事务标识符，
	 * 以便与 fxid 比较。鉴于 oldest_fxid 至多只比
	 * now_fullxid 老 2^31 以上，以定纪元。
	 */
	oldest_xid = ShmemVariableCache->oldestClogXid;
	Assert(TransactionIdPrecedesOrEquals(oldest_xid, now_epoch_next_xid));
	if (oldest_xid <= now_epoch_next_xid)
	{
		oldest_fxid = FullTransactionIdFromEpochAndXid(now_epoch, oldest_xid);
	}
	else
	{
		Assert(now_epoch > 0);
		oldest_fxid = FullTransactionIdFromEpochAndXid(now_epoch - 1, oldest_xid);
	}
	return !FullTransactionIdPrecedes(fxid, oldest_fxid);
}

/*
 * 把调用方所持快照中的 TransactionId 转换为
 * 完整事务标识符。用 next_fxid 作为参照完整事务标识符，
 * 以便我们能算出高位。它必须由调用方在快照创建
 * 之后用 ReadNextFullTransactionId() 获得。
 */
static FullTransactionId
widen_snapshot_xid(TransactionId xid, FullTransactionId next_fxid)
{
	TransactionId next_xid = XidFromFullTransactionId(next_fxid);
	uint32		epoch = EpochFromFullTransactionId(next_fxid);

	/* 特殊事务 ID。 */
	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	/*
	 * 64 位结果必然 <= next_fxid，因为创建快照时
	 * next_fxid 尚未发放。快照中的每个 TransactionId
	 * 必然与 next_fxid 同属一个纪元，或者属于其前
	 * 一个纪元。因为 next_fxid 绝不会超前任何快照中
	 * 的 TransactionId 超前一个以上纪元。
	 */
	if (xid > next_xid)
		epoch--;

	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

/*
 * qsort/bsearch 的 txid 比较器
 */
static int
cmp_fxid(const void *aa, const void *bb)
{
	FullTransactionId a = *(const FullTransactionId *) aa;
	FullTransactionId b = *(const FullTransactionId *) bb;

	if (FullTransactionIdPrecedes(a, b))
		return -1;
	if (FullTransactionIdPrecedes(b, a))
		return 1;
	return 0;
}

/*
 * 对快照的 txid 排序，以便稍后能用 bsearch()。
 * 同时去重。
 *
 * 为保证磁盘上表示的一致性，即使不用 bsearch，我们
 * 照常排序。
 */
static void
sort_snapshot(pg_snapshot *snap)
{
	if (snap->nxip > 1)
	{
		qsort(snap->xip, snap->nxip, sizeof(FullTransactionId), cmp_fxid);
		snap->nxip = qunique(snap->xip, snap->nxip, sizeof(FullTransactionId),
							 cmp_fxid);
	}
}

/*
 * 查 fxid 可见性。
 */
static bool
is_visible_fxid(FullTransactionId value, const pg_snapshot *snap)
{
	if (FullTransactionIdPrecedes(value, snap->xmin))
		return true;
	else if (!FullTransactionIdPrecedes(value, snap->xmax))
		return false;
#ifdef USE_BSEARCH_IF_NXIP_GREATER
	else if (snap->nxip > USE_BSEARCH_IF_NXIP_GREATER)
	{
		void	   *res;

		res = bsearch(&value, snap->xip, snap->nxip, sizeof(FullTransactionId),
					  cmp_fxid);
		/* 若找到，说明事务仍在进行中 */
		return (res) ? false : true;
	}
#endif
	else
	{
		uint32		i;

		for (i = 0; i < snap->nxip; i++)
		{
			if (FullTransactionIdEquals(value, snap->xip[i]))
				return false;
		}
		return true;
	}
}

/*
 * 用 StringInfo 构建 pg_snapshot 的辅助函数。
 */

static StringInfo
buf_init(FullTransactionId xmin, FullTransactionId xmax)
{
	pg_snapshot snap;
	StringInfo	buf;

	snap.xmin = xmin;
	snap.xmax = xmax;
	snap.nxip = 0;

	buf = makeStringInfo();
	appendBinaryStringInfo(buf, &snap, PG_SNAPSHOT_SIZE(0));
	return buf;
}

static void
buf_add_txid(StringInfo buf, FullTransactionId fxid)
{
	pg_snapshot *snap = (pg_snapshot *) buf->data;

	/* 在可能 realloc 前执行 */
	snap->nxip++;

	appendBinaryStringInfo(buf, &fxid, sizeof(fxid));
}

static pg_snapshot *
buf_finalize(StringInfo buf)
{
	pg_snapshot *snap = (pg_snapshot *) buf->data;

	SET_VARSIZE(snap, buf->len);

	/* 不再需要 buf 了 */
	buf->data = NULL;
	pfree(buf);

	return snap;
}

/*
 * 从 cstring 解析快照
 */
static pg_snapshot *
parse_snapshot(const char *str, Node *escontext)
{
	FullTransactionId xmin;
	FullTransactionId xmax;
	FullTransactionId last_val = InvalidFullTransactionId;
	FullTransactionId val;
	const char *str_start = str;
	char	   *endp;
	StringInfo	buf;

	xmin = FullTransactionIdFromU64(strtou64(str, &endp, 10));
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	xmax = FullTransactionIdFromU64(strtou64(str, &endp, 10));
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	/* 应显得合理 */
	if (!FullTransactionIdIsValid(xmin) ||
		!FullTransactionIdIsValid(xmax) ||
		FullTransactionIdPrecedes(xmax, xmin))
		goto bad_format;

	/* 分配缓冲区 */
	buf = buf_init(xmin, xmax);

	/* 遍历各个值 */
	while (*str != '\0')
	{
		/* 读下一个值 */
		val = FullTransactionIdFromU64(strtou64(str, &endp, 10));
		str = endp;

		/* 要求输入必须有序 */
		if (FullTransactionIdPrecedes(val, xmin) ||
			FullTransactionIdFollowsOrEquals(val, xmax) ||
			FullTransactionIdPrecedes(val, last_val))
			goto bad_format;

		/* 跳过重复项 */
		if (!FullTransactionIdEquals(val, last_val))
			buf_add_txid(buf, val);
		last_val = val;

		if (*str == ',')
			str++;
		else if (*str != '\0')
			goto bad_format;
	}

	return buf_finalize(buf);

bad_format:
	ereturn(escontext, NULL,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"pg_snapshot", str_start)));
}

/*
 * pg_current_xact_id() 返回 xid8
 *
 *	返回当前顶层完整事务标识符。
 *	若当前事务尚没有，则分配一个。
 */
Datum
pg_current_xact_id(PG_FUNCTION_ARGS)
{
	/*
	 * 恢复期必须禁止调用，因为若尚未分配 xid，我们会
	 * 尝试去分配，而这会失败。现有程序已依赖此函数
	 * 总是返回有效的当前 xid，因此不应把它改成
	 * 返回 NULL 或类似无效 xid。
	 */
	PreventCommandDuringRecovery("pg_current_xact_id()");

	PG_RETURN_FULLTRANSACTIONID(GetTopFullTransactionId());
}

/*
 * 与 pg_current_xact_id() 相同，但若还没有 xid，
 * 不分配。
 */
Datum
pg_current_xact_id_if_assigned(PG_FUNCTION_ARGS)
{
	FullTransactionId topfxid = GetTopFullTransactionIdIfAny();

	if (!FullTransactionIdIsValid(topfxid))
		PG_RETURN_NULL();

	PG_RETURN_FULLTRANSACTIONID(topfxid);
}

/*
 * pg_current_snapshot() 返回 pg_snapshot
 *
 *		返回当前快照
 *
 * 注意快照中只包含顶层事务的 XID。
 */
Datum
pg_current_snapshot(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap;
	uint32		nxip,
				i;
	Snapshot	cur;
	FullTransactionId next_fxid = ReadNextFullTransactionId();

	cur = GetActiveSnapshot();
	if (cur == NULL)
		elog(ERROR, "no active snapshot set");

	/* 分配 */
	nxip = cur->xcnt;
	snap = palloc(PG_SNAPSHOT_SIZE(nxip));

	/* 填 */
	snap->xmin = widen_snapshot_xid(cur->xmin, next_fxid);
	snap->xmax = widen_snapshot_xid(cur->xmax, next_fxid);
	snap->nxip = nxip;
	for (i = 0; i < nxip; i++)
		snap->xip[i] = widen_snapshot_xid(cur->xip[i], next_fxid);

	/*
	 * 我们要保证它们按升序排列。这同时还能去除
	 * 重复的 xid。通常一个 XID 只会分配给一个
	 * 后端，但两阶段提交准备事务时会有过渡状态，
	 * 此时原后端与为预备事务保留的哑 PGPROC 条目
	 * 都持有同一个 XID。
	 */
	sort_snapshot(snap);

	/* 排序后再设置大小，因为可能已去除了重复的 xip */
	SET_VARSIZE(snap, PG_SNAPSHOT_SIZE(snap->nxip));

	PG_RETURN_POINTER(snap);
}

/*
 * pg_snapshot_in(cstring) 返回 pg_snapshot
 *
 *		pg_snapshot 类型的输入函数
 */
Datum
pg_snapshot_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	pg_snapshot *snap;

	snap = parse_snapshot(str, fcinfo->context);

	PG_RETURN_POINTER(snap);
}

/*
 * pg_snapshot_out(pg_snapshot) 返回 cstring
 *
 *		pg_snapshot 类型的输出函数
 */
Datum
pg_snapshot_out(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData str;
	uint32		i;

	initStringInfo(&str);

	appendStringInfo(&str, UINT64_FORMAT ":",
					 U64FromFullTransactionId(snap->xmin));
	appendStringInfo(&str, UINT64_FORMAT ":",
					 U64FromFullTransactionId(snap->xmax));

	for (i = 0; i < snap->nxip; i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, ',');
		appendStringInfo(&str, UINT64_FORMAT,
						 U64FromFullTransactionId(snap->xip[i]));
	}

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_snapshot_recv(internal) 返回 pg_snapshot
 *
 *		pg_snapshot 类型的二进制输入函数
 *
 *		格式: int4 nxip, int8 xmin, int8 xmax, int8 xip
 */
Datum
pg_snapshot_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	pg_snapshot *snap;
	FullTransactionId last = InvalidFullTransactionId;
	int			nxip;
	int			i;
	FullTransactionId xmin;
	FullTransactionId xmax;

	/* 读取并校验 nxip */
	nxip = pq_getmsgint(buf, 4);
	if (nxip < 0 || nxip > PG_SNAPSHOT_MAX_NXIP)
		goto bad_format;

	xmin = FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));
	xmax = FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));
	if (!FullTransactionIdIsValid(xmin) ||
		!FullTransactionIdIsValid(xmax) ||
		FullTransactionIdPrecedes(xmax, xmin))
		goto bad_format;

	snap = palloc(PG_SNAPSHOT_SIZE(nxip));
	snap->xmin = xmin;
	snap->xmax = xmax;

	for (i = 0; i < nxip; i++)
	{
		FullTransactionId cur =
			FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));

		if (FullTransactionIdPrecedes(cur, last) ||
			FullTransactionIdPrecedes(cur, xmin) ||
			FullTransactionIdPrecedes(xmax, cur))
			goto bad_format;

		/* 跳过重复的 xip */
		if (FullTransactionIdEquals(cur, last))
		{
			i--;
			nxip--;
			continue;
		}

		snap->xip[i] = cur;
		last = cur;
	}
	snap->nxip = nxip;
	SET_VARSIZE(snap, PG_SNAPSHOT_SIZE(nxip));
	PG_RETURN_POINTER(snap);

bad_format:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			 errmsg("invalid external pg_snapshot data")));
	PG_RETURN_POINTER(NULL);	/* 使编译器安静 */
}

/*
 * pg_snapshot_send(pg_snapshot) 返回 bytea
 *
 *		pg_snapshot 类型的二进制输出函数
 *
 *		格式: int4 nxip, u64 xmin, u64 xmax, u64 xip...
 */
Datum
pg_snapshot_send(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData buf;
	uint32		i;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, snap->nxip);
	pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xmin));
	pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xmax));
	for (i = 0; i < snap->nxip; i++)
		pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xip[i]));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * pg_visible_in_snapshot(xid8, pg_snapshot) 返回 bool
 *
 *		txid 在快照中可见吗？
 */
Datum
pg_visible_in_snapshot(PG_FUNCTION_ARGS)
{
	FullTransactionId value = PG_GETARG_FULLTRANSACTIONID(0);
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(1);

	PG_RETURN_BOOL(is_visible_fxid(value, snap));
}

/*
 * pg_snapshot_xmin(pg_snapshot) 返回 xid8
 *
 *		返回快照的 xmin
 */
Datum
pg_snapshot_xmin(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_FULLTRANSACTIONID(snap->xmin);
}

/*
 * pg_snapshot_xmax(pg_snapshot) 返回 xid8
 *
 *		返回快照的 xmax
 */
Datum
pg_snapshot_xmax(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_FULLTRANSACTIONID(snap->xmax);
}

/*
 * pg_snapshot_xip(pg_snapshot) 返回 setof xid8
 *
 *		返回快照中进行中的 xid8。
 */
Datum
pg_snapshot_xip(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	pg_snapshot *snap;
	FullTransactionId value;

	/* 首次调用时初始化 fctx 并获取快照副本 */
	if (SRF_IS_FIRSTCALL())
	{
		pg_snapshot *arg = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

		fctx = SRF_FIRSTCALL_INIT();

		/* 复制一份用户快照 */
		snap = MemoryContextAlloc(fctx->multi_call_memory_ctx, VARSIZE(arg));
		memcpy(snap, arg, VARSIZE(arg));

		fctx->user_fctx = snap;
	}

	/* 逐个返回值 */
	fctx = SRF_PERCALL_SETUP();
	snap = fctx->user_fctx;
	if (fctx->call_cntr < snap->nxip)
	{
		value = snap->xip[fctx->call_cntr];
		SRF_RETURN_NEXT(fctx, FullTransactionIdGetDatum(value));
	}
	else
	{
		SRF_RETURN_DONE(fctx);
	}
}

/*
 * 报告较新事务 ID 的状态；对已回绕、
 * 已截除或过老的 XID 则为 null。
 *
 * 传入的带纪元限定的 xid 被当作普通 xid 处理，
 * 非 mxid。
 *
 * 若指向已提交的子事务，结果就是该子事务的状态，
 * 即使父事务可能仍在进行中或已中止。
 */
Datum
pg_xact_status(PG_FUNCTION_ARGS)
{
	const char *status;
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);
	TransactionId xid;

	/*
	 * 必须防范 clog 条目被并发截断，以避免
	 * SLRU 查找的 I/O 错误。
	 */
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	if (TransactionIdInRecentPast(fxid, &xid))
	{
		Assert(TransactionIdIsValid(xid));

		/*
		 * 与对行做可见性检查时一样，先检查事务
		 * 是否仍在进行中，然后再查 CLOG。
		 * 否则，对一个正在提交且已更新 CLOG、但尚未从
		 * proc 数组移除其 XID 的事务，我们会错误地返回
		 * "committed"。（有关该竞争条件的说明见
		 * heapam_visibility.c 顶部）。
		 */
		if (TransactionIdIsInProgress(xid))
			status = "in progress";
		else if (TransactionIdDidCommit(xid))
			status = "committed";
		else
		{
			/* 它必定已中止或崩溃 */
			status = "aborted";
		}
	}
	else
	{
		status = NULL;
	}
	LWLockRelease(XactTruncationLock);

	if (status == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_TEXT_P(cstring_to_text(status));
}
