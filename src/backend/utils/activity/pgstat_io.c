/* -------------------------------------------------------------------------
 *
 * pgstat_io.c
 *	  IO 统计的实现。
 *
 * 本文件包含 IO 统计的实现。它与 pgstat.c 分开，
 * 是为了在统计的访问 / 存储实现和各类统计的
 * 细节之间划清清楚的界限。
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_io.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/instrument.h"
#include "storage/bufmgr.h"
#include "utils/pgstat_internal.h"


typedef struct PgStat_PendingIO
{
	PgStat_Counter counts[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
	instr_time	pending_times[IOOBJECT_NUM_TYPES][IOCONTEXT_NUM_TYPES][IOOP_NUM_TYPES];
} PgStat_PendingIO;


static PgStat_PendingIO PendingIOStats;
bool		have_iostats = false;


/*
 * 检查对于传入 BackendType 不跟踪的任何 IOObject、
 * IOContext 与 IOOp 组合是否有统计被计入。若该组合
 * 有统计且 IO 时间非零，则计数也应
 * 非零。
 *
 * 传入的 PgStat_BktypeIO 必须包含第二个参数所
 * 指定 BackendType 的统计。调用者负责在需要时
 * 锁定传入的 PgStat_BktypeIO。
 */
bool
pgstat_bktype_io_stats_valid(PgStat_BktypeIO *backend_io,
							 BackendType bktype)
{
	for (int io_object = 0; io_object < IOOBJECT_NUM_TYPES; io_object++)
	{
		for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
		{
			for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
			{
				/* 有跟踪 */
				if (pgstat_tracks_io_op(bktype, io_object, io_context, io_op))
				{
					/* 确保 IO 时间非零时计数 > 0 */
					if (backend_io->times[io_object][io_context][io_op] != 0 &&
						backend_io->counts[io_object][io_context][io_op] <= 0)
						return false;

					continue;
				}

				/* 不跟踪它，且它非 0 */
				if (backend_io->counts[io_object][io_context][io_op] != 0)
					return false;
			}
		}
	}

	return true;
}

void
pgstat_count_io_op(IOObject io_object, IOContext io_context, IOOp io_op)
{
	pgstat_count_io_op_n(io_object, io_context, io_op, 1);
}

void
pgstat_count_io_op_n(IOObject io_object, IOContext io_context, IOOp io_op, uint32 cnt)
{
	Assert((unsigned int) io_object < IOOBJECT_NUM_TYPES);
	Assert((unsigned int) io_context < IOCONTEXT_NUM_TYPES);
	Assert((unsigned int) io_op < IOOP_NUM_TYPES);
	Assert(pgstat_tracks_io_op(MyBackendType, io_object, io_context, io_op));

	PendingIOStats.counts[io_object][io_context][io_op] += cnt;

	have_iostats = true;
}

instr_time
pgstat_prepare_io_time(void)
{
	instr_time	io_start;

	if (track_io_timing)
		INSTR_TIME_SET_CURRENT(io_start);
	else
		INSTR_TIME_SET_ZERO(io_start);

	return io_start;
}

/*
 * 与 pgstat_count_io_op_n() 类似，但还会累计时间。
 */
void
pgstat_count_io_op_time(IOObject io_object, IOContext io_context, IOOp io_op,
						instr_time start_time, uint32 cnt)
{
	if (track_io_timing)
	{
		instr_time	io_time;

		INSTR_TIME_SET_CURRENT(io_time);
		INSTR_TIME_SUBTRACT(io_time, start_time);

		if (io_op == IOOP_WRITE || io_op == IOOP_EXTEND)
		{
			pgstat_count_buffer_write_time(INSTR_TIME_GET_MICROSEC(io_time));
			if (io_object == IOOBJECT_RELATION)
				INSTR_TIME_ADD(pgBufferUsage.blk_write_time, io_time);
		}
		else if (io_op == IOOP_READ)
		{
			pgstat_count_buffer_read_time(INSTR_TIME_GET_MICROSEC(io_time));
			if (io_object == IOOBJECT_RELATION)
				INSTR_TIME_ADD(pgBufferUsage.blk_read_time, io_time);
		}

		INSTR_TIME_ADD(PendingIOStats.pending_times[io_object][io_context][io_op],
					   io_time);
	}

	pgstat_count_io_op_n(io_object, io_context, io_op, cnt);
}

PgStat_IO *
pgstat_fetch_stat_io(void)
{
	pgstat_snapshot_fixed(PGSTAT_KIND_IO);

	return &pgStatLocal.snapshot.io;
}

/*
 * 刷出本地待写入的 IO 统计
 *
 * 若未记录任何统计，本函数返回 false。
 *
 * 若 nowait 为 true，则在无法获得锁时本函数返回
 * true，否则返回 false。
 */
bool
pgstat_flush_io(bool nowait)
{
	LWLock	   *bktype_lock;
	PgStat_BktypeIO *bktype_shstats;

	if (!have_iostats)
		return false;

	bktype_lock = &pgStatLocal.shmem->io.locks[MyBackendType];
	bktype_shstats =
		&pgStatLocal.shmem->io.stats.stats[MyBackendType];

	if (!nowait)
		LWLockAcquire(bktype_lock, LW_EXCLUSIVE);
	else if (!LWLockConditionalAcquire(bktype_lock, LW_EXCLUSIVE))
		return true;

	for (int io_object = 0; io_object < IOOBJECT_NUM_TYPES; io_object++)
	{
		for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
		{
			for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
			{
				instr_time	time;

				bktype_shstats->counts[io_object][io_context][io_op] +=
					PendingIOStats.counts[io_object][io_context][io_op];

				time = PendingIOStats.pending_times[io_object][io_context][io_op];

				bktype_shstats->times[io_object][io_context][io_op] +=
					INSTR_TIME_GET_MICROSEC(time);
			}
		}
	}

	Assert(pgstat_bktype_io_stats_valid(bktype_shstats, MyBackendType));

	LWLockRelease(bktype_lock);

	memset(&PendingIOStats, 0, sizeof(PendingIOStats));

	have_iostats = false;

	return false;
}

const char *
pgstat_get_io_context_name(IOContext io_context)
{
	switch (io_context)
	{
		case IOCONTEXT_BULKREAD:
			return "bulkread";
		case IOCONTEXT_BULKWRITE:
			return "bulkwrite";
		case IOCONTEXT_NORMAL:
			return "normal";
		case IOCONTEXT_VACUUM:
			return "vacuum";
	}

	elog(ERROR, "unrecognized IOContext value: %d", io_context);
	pg_unreachable();
}

const char *
pgstat_get_io_object_name(IOObject io_object)
{
	switch (io_object)
	{
		case IOOBJECT_RELATION:
			return "relation";
		case IOOBJECT_TEMP_RELATION:
			return "temp relation";
	}

	elog(ERROR, "unrecognized IOObject value: %d", io_object);
	pg_unreachable();
}

void
pgstat_io_reset_all_cb(TimestampTz ts)
{
	for (int i = 0; i < BACKEND_NUM_TYPES; i++)
	{
		LWLock	   *bktype_lock = &pgStatLocal.shmem->io.locks[i];
		PgStat_BktypeIO *bktype_shstats = &pgStatLocal.shmem->io.stats.stats[i];

		LWLockAcquire(bktype_lock, LW_EXCLUSIVE);

		/*
		 * 使用第一个 BackendType 的 PgStat_BktypeIO 中的锁
		 * 并保护重置时间戳。
		 */
		if (i == 0)
			pgStatLocal.shmem->io.stats.stat_reset_timestamp = ts;

		memset(bktype_shstats, 0, sizeof(*bktype_shstats));
		LWLockRelease(bktype_lock);
	}
}

void
pgstat_io_snapshot_cb(void)
{
	for (int i = 0; i < BACKEND_NUM_TYPES; i++)
	{
		LWLock	   *bktype_lock = &pgStatLocal.shmem->io.locks[i];
		PgStat_BktypeIO *bktype_shstats = &pgStatLocal.shmem->io.stats.stats[i];
		PgStat_BktypeIO *bktype_snap = &pgStatLocal.snapshot.io.stats[i];

		LWLockAcquire(bktype_lock, LW_SHARED);

		/*
		 * 使用第一个 BackendType 的 PgStat_BktypeIO 中的锁
		 * 并保护重置时间戳。
		 */
		if (i == 0)
			pgStatLocal.snapshot.io.stat_reset_timestamp =
				pgStatLocal.shmem->io.stats.stat_reset_timestamp;

		/* 用结构体赋值以获得更好的类型安全 */
		*bktype_snap = *bktype_shstats;
		LWLockRelease(bktype_lock);
	}
}

/*
* 并非所有 BackendType 都收集 IO 统计。
*
* 以下 BackendType 不参与累计统计子系统，或未执行
* 我们目前跟踪的 IO。
* - Syslogger：未连接到共享内存
* - Archiver：大部分相关归档 IO 委托给了专门
*   命令或模块。
* - WAL Receiver 与 WAL Writer 的 IO 暂不在 pg_stat_io 中跟踪
*
* 若 BackendType 参与 IO 的累计统计子系统则函数返回
* true，否则返回 false。
*
* 新增 BackendType 时，也应考虑向 pgstat_tracks_io_object()
* 和 pgstat_tracks_io_op() 添加相关限制。
*/
bool
pgstat_tracks_io_bktype(BackendType bktype)
{
	/*
	 * 逐一列出各类型，让新后端类型触发需要调整
	 * 此 switch 的警告。
	 */
	switch (bktype)
	{
		case B_INVALID:
		case B_ARCHIVER:
		case B_LOGGER:
		case B_WAL_RECEIVER:
		case B_WAL_WRITER:
			return false;

		case B_AUTOVAC_LAUNCHER:
		case B_AUTOVAC_WORKER:
		case B_BACKEND:
		case B_BG_WORKER:
		case B_BG_WRITER:
		case B_CHECKPOINTER:
		case B_STANDALONE_BACKEND:
		case B_STARTUP:
		case B_WAL_SENDER:
			return true;
	}

	return false;
}

/*
 * 某些 BackendType 不会对某些 IOObject 执行 IO，
 * 也不会在某些 IOContext 中执行。某些 IOObject 在某些
 * IOContext 中绝不被操作。检查给定的 BackendType
 * 否预期在给定 IOContext 与 IOObject 上执行 IO，且给定 IOObject
 * 于该 IOContext 被操作
 */
bool
pgstat_tracks_io_object(BackendType bktype, IOObject io_object,
						IOContext io_context)
{
	bool		no_temp_rel;

	/*
	 * 某些 BackendType 绝不应跟踪 IO 统计。
	 */
	if (!pgstat_tracks_io_bktype(bktype))
		return false;

	/*
	 * 目前，临时关系上的 IO 只会发生在
	 * IOCONTEXT_NORMAL IOContext
	 */
	if (io_context != IOCONTEXT_NORMAL &&
		io_object == IOOBJECT_TEMP_RELATION)
		return false;

	/*
	 * 在核心 Postgres 中，只有普通后台进程和执行查询
	 * 的 WAL Sender 进程会使用本地缓冲区并操作临时
	 * 关系。并行工作进程不用本地缓冲区（参见
	 * InitLocalBuffers()）；但利用后台工作进程的扩展
	 * 没有此限制，因此为 BackendType B_BG_WORKER 统计
	 * IOOBJECT_TEMP_RELATION
	 */
	no_temp_rel = bktype == B_AUTOVAC_LAUNCHER || bktype == B_BG_WRITER ||
		bktype == B_CHECKPOINTER || bktype == B_AUTOVAC_WORKER ||
		bktype == B_STANDALONE_BACKEND || bktype == B_STARTUP;

	if (no_temp_rel && io_context == IOCONTEXT_NORMAL &&
		io_object == IOOBJECT_TEMP_RELATION)
		return false;

	/*
	 * 某些 BackendType 目前在某些 IOContext 中不执行
	 * 任何 IO，虽然它们这样做未必天然不正确，但将这
	 * 些行从视图中排除能让视图更易于使用。
	 */
	if ((bktype == B_CHECKPOINTER || bktype == B_BG_WRITER) &&
		(io_context == IOCONTEXT_BULKREAD ||
		 io_context == IOCONTEXT_BULKWRITE ||
		 io_context == IOCONTEXT_VACUUM))
		return false;

	if (bktype == B_AUTOVAC_LAUNCHER && io_context == IOCONTEXT_VACUUM)
		return false;

	if ((bktype == B_AUTOVAC_WORKER || bktype == B_AUTOVAC_LAUNCHER) &&
		io_context == IOCONTEXT_BULKWRITE)
		return false;

	return true;
}

/*
 * 某些 BackendType 绝不会执行某些 IOOp，且某些 IOOp
 * 不应发生在某些 IOContext 或某些 IOObject 中。检查
 * 给定的 IOOp 在给定 IOContext 中、对给定 IOObject 而
 * 言是否有效。注意目前没有任何 IOOp 仅在某个
 * IOContext 内、或仅对某个 IOObject 而言对特定
 * BackendType 无效。
 */
bool
pgstat_tracks_io_op(BackendType bktype, IOObject io_object,
					IOContext io_context, IOOp io_op)
{
	bool		strategy_io_context;

	/* 若 (io_context, io_object) 永不收集统计，即完成 */
	if (!pgstat_tracks_io_object(bktype, io_object, io_context))
		return false;

	/*
	 * 某些 BackendType 不执行某些 IOOp。
	 */
	if ((bktype == B_BG_WRITER || bktype == B_CHECKPOINTER) &&
		(io_op == IOOP_READ || io_op == IOOP_EVICT || io_op == IOOP_HIT))
		return false;

	if ((bktype == B_AUTOVAC_LAUNCHER || bktype == B_BG_WRITER ||
		 bktype == B_CHECKPOINTER) && io_op == IOOP_EXTEND)
		return false;

	/*
	 * 临时表不记录 WAL，因此不需要 fsync。
	 * 临时表不会请求回写。
	 */
	if (io_object == IOOBJECT_TEMP_RELATION &&
		(io_op == IOOP_FSYNC || io_op == IOOP_WRITEBACK))
		return false;

	/*
	 * 某些 IOOp 在某些 IOContext 中无效，而某些 IOOp 只
	 * 在特定上下文有效
	 */
	if (io_context == IOCONTEXT_BULKREAD && io_op == IOOP_EXTEND)
		return false;

	strategy_io_context = io_context == IOCONTEXT_BULKREAD ||
		io_context == IOCONTEXT_BULKWRITE || io_context == IOCONTEXT_VACUUM;

	/*
	 * IOOP_REUSE 仅在使用 BufferAccessStrategy 时才有意义。
	 */
	if (!strategy_io_context && io_op == IOOP_REUSE)
		return false;

	/*
	 * 使用 BufferAccessStrategy 的后端执行的 IOOP_FSYNC
	 * IOOp 会计入 IOCONTEXT_NORMAL IOContext。详见
	 * register_dirty_segment() 中的注释。
	 */
	if (strategy_io_context && io_op == IOOP_FSYNC)
		return false;


	return true;
}
