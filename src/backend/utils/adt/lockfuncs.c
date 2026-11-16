/*-------------------------------------------------------------------------
 *
 * lockfuncs.c
 *		以 SQL 访问各种锁管理器能力的函数。
 *
 * Copyright (c) 2002-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/backend/utils/adt/lockfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/predicate_internals.h"
#include "utils/array.h"
#include "utils/builtins.h"


/*
 * 此处必须与枚举 LockTagType 一致！且务必把任何变更
 * 记入 pg_locks 视图与等待事件类型的文档中。
 */
const char *const LockTagTypeNames[] = {
	"relation",
	"extend",
	"frozenid",
	"page",
	"tuple",
	"transactionid",
	"virtualxid",
	"spectoken",
	"object",
	"userlock",
	"advisory",
	"applytransaction"
};

StaticAssertDecl(lengthof(LockTagTypeNames) == (LOCKTAG_LAST_TYPE + 1),
				 "array length mismatch");

/* 须匹配枚举 PredicateLockTargetType（predicate_internals.h） */
static const char *const PredicateLockTagTypeNames[] = {
	"relation",
	"page",
	"tuple"
};

StaticAssertDecl(lengthof(PredicateLockTagTypeNames) == (PREDLOCKTAG_TUPLE + 1),
				 "array length mismatch");

/* pg_lock_status 的工作状态 */
typedef struct
{
	LockData   *lockData;		/* lmgr 的状态数据 */
	int			currIdx;		/* 当前 PROCLOCK 索引 */
	PredicateLockData *predLockData;	/* 谓词锁的状态数据 */
	int			predLockIdx;	/* 谓词锁的当前索引 */
} PG_Lock_Status;

/* pg_locks 输出的列数 */
#define NUM_LOCK_STATUS_COLUMNS		16

/*
 * VXIDGetDatum - 构造 VXID 的文本表示
 *
 * 目前只有 pg_lock_status 使用它，所以放在这里。
 */
static Datum
VXIDGetDatum(BackendId bid, LocalTransactionId lxid)
{
	/*
	 * 表示形式为 "<bid>/<lxid>"，分别为十进制和
	 * 无符号十进制。注意 elog.c 亦会格式化 vxid。
	 */
	char		vxidstr[32];

	snprintf(vxidstr, sizeof(vxidstr), "%d/%u", bid, lxid);

	return CStringGetTextDatum(vxidstr);
}


/*
 * pg_lock_status - 生成视图，每行一个持有或等待的锁模式
 */
Datum
pg_lock_status(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	PG_Lock_Status *mystatus;
	LockData   *lockData;
	PredicateLockData *predLockData;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* 创建用于跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * 切换到适合多次函数调用的内存上下文
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* 为结果元组构建 tupdesc */
		/* 此声明最好与 pg_proc.h 中的函数声明一致 */
		tupdesc = CreateTemplateTupleDesc(NUM_LOCK_STATUS_COLUMNS);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "locktype",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "database",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "relation",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "page",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "tuple",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "virtualxid",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "transactionid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "classid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "objid",
						   OIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "objsubid",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "virtualtransaction",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "pid",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "mode",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "granted",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 15, "fastpath",
						   BOOLOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 16, "waitstart",
						   TIMESTAMPTZOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		/*
		 * 收集所有将格式化并作为结果集发出的锁信息。
		 * 
		 */
		mystatus = (PG_Lock_Status *) palloc(sizeof(PG_Lock_Status));
		funcctx->user_fctx = (void *) mystatus;

		mystatus->lockData = GetLockStatusData();
		mystatus->currIdx = 0;
		mystatus->predLockData = GetPredicateLockStatusData();
		mystatus->predLockIdx = 0;

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	mystatus = (PG_Lock_Status *) funcctx->user_fctx;
	lockData = mystatus->lockData;

	while (mystatus->currIdx < lockData->nelements)
	{
		bool		granted;
		LOCKMODE	mode = 0;
		const char *locktypename;
		char		tnbuf[32];
		Datum		values[NUM_LOCK_STATUS_COLUMNS] = {0};
		bool		nulls[NUM_LOCK_STATUS_COLUMNS] = {0};
		HeapTuple	tuple;
		Datum		result;
		LockInstanceData *instance;

		instance = &(lockData->locks[mystatus->currIdx]);

		/*
		 * 查看此 PROCLOCK 中是否还有持有的锁模式。若有，
		 * 就上报，并破坏性地修改 lockData 以免重复上报。
		 * 
		 */
		granted = false;
		if (instance->holdMask)
		{
			for (mode = 0; mode < MAX_LOCKMODES; mode++)
			{
				if (instance->holdMask & LOCKBIT_ON(mode))
				{
					granted = true;
					instance->holdMask &= LOCKBIT_OFF(mode);
					break;
				}
			}
		}

		/*
		 * 若无（更多）持有模式可上报，则看 PROC 是否正
		 * 等待此锁。
		 */
		if (!granted)
		{
			if (instance->waitLockMode != NoLock)
			{
				/* 是的，以适当的模式上报 */
				mode = instance->waitLockMode;

				/*
				 * 本 PROCLOCK 已处理完毕，因此前移指针，
				 * 下次调用时处理下一个。
				 */
				mystatus->currIdx++;
			}
			else
			{
				/*
				 * 好，与该 PROCLOCK 关联的锁都已显示完毕，
				 * 继续处理下一个。
				 */
				mystatus->currIdx++;
				continue;
			}
		}

		/*
		 * 用适当数据构造元组。
		 */

		if (instance->locktag.locktag_type <= LOCKTAG_LAST_TYPE)
			locktypename = LockTagTypeNames[instance->locktag.locktag_type];
		else
		{
			snprintf(tnbuf, sizeof(tnbuf), "unknown %d",
					 (int) instance->locktag.locktag_type);
			locktypename = tnbuf;
		}
		values[0] = CStringGetTextDatum(locktypename);

		switch ((LockTagType) instance->locktag.locktag_type)
		{
			case LOCKTAG_RELATION:
			case LOCKTAG_RELATION_EXTEND:
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				values[2] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[6] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_DATABASE_FROZEN_IDS:
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[6] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_PAGE:
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				values[2] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				values[3] = UInt32GetDatum(instance->locktag.locktag_field3);
				nulls[4] = true;
				nulls[5] = true;
				nulls[6] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_TUPLE:
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				values[2] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				values[3] = UInt32GetDatum(instance->locktag.locktag_field3);
				values[4] = UInt16GetDatum(instance->locktag.locktag_field4);
				nulls[5] = true;
				nulls[6] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_TRANSACTION:
				values[6] =
					TransactionIdGetDatum(instance->locktag.locktag_field1);
				nulls[1] = true;
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_VIRTUALTRANSACTION:
				values[5] = VXIDGetDatum(instance->locktag.locktag_field1,
										 instance->locktag.locktag_field2);
				nulls[1] = true;
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[6] = true;
				nulls[7] = true;
				nulls[8] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_SPECULATIVE_TOKEN:
				values[6] =
					TransactionIdGetDatum(instance->locktag.locktag_field1);
				values[8] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				nulls[1] = true;
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[7] = true;
				nulls[9] = true;
				break;
			case LOCKTAG_APPLY_TRANSACTION:
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				values[8] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				values[6] = ObjectIdGetDatum(instance->locktag.locktag_field3);
				values[9] = Int16GetDatum(instance->locktag.locktag_field4);
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[7] = true;
				break;
			case LOCKTAG_OBJECT:
			case LOCKTAG_USERLOCK:
			case LOCKTAG_ADVISORY:
			default:			/* 把未知 locktag 视为 OBJECT */
				values[1] = ObjectIdGetDatum(instance->locktag.locktag_field1);
				values[7] = ObjectIdGetDatum(instance->locktag.locktag_field2);
				values[8] = ObjectIdGetDatum(instance->locktag.locktag_field3);
				values[9] = Int16GetDatum(instance->locktag.locktag_field4);
				nulls[2] = true;
				nulls[3] = true;
				nulls[4] = true;
				nulls[5] = true;
				nulls[6] = true;
				break;
		}

		values[10] = VXIDGetDatum(instance->backend, instance->lxid);
		if (instance->pid != 0)
			values[11] = Int32GetDatum(instance->pid);
		else
			nulls[11] = true;
		values[12] = CStringGetTextDatum(GetLockmodeName(instance->locktag.locktag_lockmethodid, mode));
		values[13] = BoolGetDatum(granted);
		values[14] = BoolGetDatum(instance->fastpath);
		if (!granted && instance->waitStart != 0)
			values[15] = TimestampTzGetDatum(instance->waitStart);
		else
			nulls[15] = true;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	/*
	 * 已返回所有常规锁。现在开始处理 SIREAD 谓词锁。
	 * 
	 */
	predLockData = mystatus->predLockData;
	if (mystatus->predLockIdx < predLockData->nelements)
	{
		PredicateLockTargetType lockType;

		PREDICATELOCKTARGETTAG *predTag = &(predLockData->locktags[mystatus->predLockIdx]);
		SERIALIZABLEXACT *xact = &(predLockData->xacts[mystatus->predLockIdx]);
		Datum		values[NUM_LOCK_STATUS_COLUMNS] = {0};
		bool		nulls[NUM_LOCK_STATUS_COLUMNS] = {0};
		HeapTuple	tuple;
		Datum		result;

		mystatus->predLockIdx++;

		/*
		 * 用适当数据构造元组。
		 */

		/* 锁类型 */
		lockType = GET_PREDICATELOCKTARGETTAG_TYPE(*predTag);

		values[0] = CStringGetTextDatum(PredicateLockTagTypeNames[lockType]);

		/* 锁目标 */
		values[1] = GET_PREDICATELOCKTARGETTAG_DB(*predTag);
		values[2] = GET_PREDICATELOCKTARGETTAG_RELATION(*predTag);
		if (lockType == PREDLOCKTAG_TUPLE)
			values[4] = GET_PREDICATELOCKTARGETTAG_OFFSET(*predTag);
		else
			nulls[4] = true;
		if ((lockType == PREDLOCKTAG_TUPLE) ||
			(lockType == PREDLOCKTAG_PAGE))
			values[3] = GET_PREDICATELOCKTARGETTAG_PAGE(*predTag);
		else
			nulls[3] = true;

		/* 这些字段是其他类型锁的目标 */
		nulls[5] = true;		/* virtualxid */
		nulls[6] = true;		/* transactionid */
		nulls[7] = true;		/* classid */
		nulls[8] = true;		/* objid */
		nulls[9] = true;		/* objsubid */

		/* 持锁者 */
		values[10] = VXIDGetDatum(xact->vxid.backendId,
								  xact->vxid.localTransactionId);
		if (xact->pid != 0)
			values[11] = Int32GetDatum(xact->pid);
		else
			nulls[11] = true;

		/*
		 * 锁模式。当前所有谓词锁都是 SIReadLocks，它们
		 * 总是持有（从不等待）且无快速路径
		 */
		values[12] = CStringGetTextDatum("SIReadLock");
		values[13] = BoolGetDatum(true);
		values[14] = BoolGetDatum(false);
		nulls[15] = true;

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}


/*
 * pg_blocking_pids - 生成阻塞给定 PID 的 PID 数组
 *
 * 上报的 PID 是那些持有与 blocked_pid 当前请求相冲突的
 * 锁的进程（硬阻塞），或是正在请求这样的锁且在该锁
 * 等待队列中先于 blocked_pid（软阻塞）。
 *
 * 在并行查询场景下，我们上报阻塞给定 PID 锁组任一
 * 成员的所有 PID，且上报的 PID 是阻塞方
 * 锁组组长的 PID。这使得调用方可以把结果与客户端
 * 的 pg_backend_pid() 结果列表比较，即便在并行查询期间。
 *
 * 并行查询使结果中可能出现重复的 PID，或因多个
 * 等待者被同一 PID 阻塞，或是因为多个阻塞方的
 * 组长 PID 相同。我们不打算费心去把
 * 这类重复项从结果中剔除。
 *
 * 此处无需考虑谓词锁，因为谓词锁不会阻塞任何东西。
 */
Datum
pg_blocking_pids(PG_FUNCTION_ARGS)
{
	int			blocked_pid = PG_GETARG_INT32(0);
	Datum	   *arrayelems;
	int			narrayelems;
	BlockedProcsData *lockData; /* lmgr 的状态数据 */
	int			i,
				j;

	/* 收集锁管理器状态的快照 */
	lockData = GetBlockerStatusData(blocked_pid);

	/* 所需的输出条目不会多于已上报的 PROCLOCK 数 */
	arrayelems = (Datum *) palloc(lockData->nlocks * sizeof(Datum));
	narrayelems = 0;

	/* 对锁组中的每个被阻塞进程 ... */
	for (i = 0; i < lockData->nprocs; i++)
	{
		BlockedProcData *bproc = &lockData->procs[i];
		LockInstanceData *instances = &lockData->locks[bproc->first_lock];
		int		   *preceding_waiters = &lockData->waiter_pids[bproc->first_waiter];
		LockInstanceData *blocked_instance;
		LockMethod	lockMethodTable;
		int			conflictMask;

		/*
		 * 在 LockInstanceData 数组中定位被阻塞进程自身条目。
		 * 应恰好有一个匹配的条目。
		 */
		blocked_instance = NULL;
		for (j = 0; j < bproc->num_locks; j++)
		{
			LockInstanceData *instance = &(instances[j]);

			if (instance->pid == bproc->pid)
			{
				Assert(blocked_instance == NULL);
				blocked_instance = instance;
			}
		}
		Assert(blocked_instance != NULL);

		lockMethodTable = GetLockTagsMethodTable(&(blocked_instance->locktag));
		conflictMask = lockMethodTable->conflictTab[blocked_instance->waitLockMode];

		/* 现在扫描 PROCLOCK 数据以找冲突进程 */
		for (j = 0; j < bproc->num_locks; j++)
		{
			LockInstanceData *instance = &(instances[j]);

			/* 进程绝不会阻塞自己，故忽略该条目 */
			if (instance == blocked_instance)
				continue;
			/* 同一锁组的成员彼此也绝不会阻塞 */
			if (instance->leaderPid == blocked_instance->leaderPid)
				continue;

			if (conflictMask & instance->holdMask)
			{
				/* 硬阻塞：被该条目已持有的锁所阻塞 */
			}
			else if (instance->waitLockMode != NoLock &&
					 (conflictMask & LOCKBIT_ON(instance->waitLockMode)))
			{
				/* 锁请求冲突；谁在等待队列的前面？ */
				bool		ahead = false;
				int			k;

				for (k = 0; k < bproc->num_waiters; k++)
				{
					if (preceding_waiters[k] == instance->pid)
					{
						/* 软阻塞：该条目领先于被阻塞进程 */
						ahead = true;
						break;
					}
				}
				if (!ahead)
					continue;	/* 未被该条目阻塞 */
			}
			else
			{
				/* 未被该条目阻塞 */
				continue;
			}

			/* 被该条目阻塞，故发出记录 */
			arrayelems[narrayelems++] = Int32GetDatum(instance->leaderPid);
		}
	}

	/* 断言没有越界写 arrayelems[] */
	Assert(narrayelems <= lockData->nlocks);

	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(arrayelems, narrayelems, INT4OID));
}


/*
 * pg_safe_snapshot_blocking_pids - 生成阻塞给定 PID、
 * 无法获得安全快照的 PID 数组
 *
 * XXX 此处未考虑并行查询场景；不清楚这在实际中
 * 会是多大的问题。
 */
Datum
pg_safe_snapshot_blocking_pids(PG_FUNCTION_ARGS)
{
	int			blocked_pid = PG_GETARG_INT32(0);
	int		   *blockers;
	int			num_blockers;
	Datum	   *blocker_datums;

	/* 足以容纳任何阻塞者列表而无须截断的缓冲区 */
	blockers = (int *) palloc(MaxBackends * sizeof(int));

	/* 收集 GetSafeSnapshot 所等待进程的快照 */
	num_blockers =
		GetSafeSnapshotBlockingPids(blocked_pid, blockers, MaxBackends);

	/* int 数组转成 Datum 数组 */
	if (num_blockers > 0)
	{
		int			i;

		blocker_datums = (Datum *) palloc(num_blockers * sizeof(Datum));
		for (i = 0; i < num_blockers; ++i)
			blocker_datums[i] = Int32GetDatum(blockers[i]);
	}
	else
		blocker_datums = NULL;

	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(blocker_datums, num_blockers, INT4OID));
}


/*
 * pg_isolation_test_session_is_blocked - isolationtester 的支持函数
 *
 * 检查指定 PID 是否被第二个参数所列 PID 中的任一个
 * 所阻塞。目前它查找因等待重量级锁或安全快照
 * 而引起的阻塞。我们忽略不受 isolationtester 直接控制
 * 的 PID 所造成的阻塞，例如 autovacuum。
 *
 * 此为未载入文档的函数，供 isolation tester 使用，
 * 并且在后续版本中可能随测试需要而变更。
 */
Datum
pg_isolation_test_session_is_blocked(PG_FUNCTION_ARGS)
{
	int			blocked_pid = PG_GETARG_INT32(0);
	ArrayType  *interesting_pids_a = PG_GETARG_ARRAYTYPE_P(1);
	ArrayType  *blocking_pids_a;
	int32	   *interesting_pids;
	int32	   *blocking_pids;
	int			num_interesting_pids;
	int			num_blocking_pids;
	int			dummy;
	int			i,
				j;

	/* 校验传入的数组 */
	Assert(ARR_ELEMTYPE(interesting_pids_a) == INT4OID);
	if (array_contains_nulls(interesting_pids_a))
		elog(ERROR, "array must not contain nulls");
	interesting_pids = (int32 *) ARR_DATA_PTR(interesting_pids_a);
	num_interesting_pids = ArrayGetNItems(ARR_NDIM(interesting_pids_a),
										  ARR_DIMS(interesting_pids_a));

	/*
	 * 取得所有在给定会话试图获取重量级锁时
	 * 阻塞它的会话 PID。
	 */
	blocking_pids_a =
		DatumGetArrayTypeP(DirectFunctionCall1(pg_blocking_pids, blocked_pid));

	Assert(ARR_ELEMTYPE(blocking_pids_a) == INT4OID);
	Assert(!array_contains_nulls(blocking_pids_a));
	blocking_pids = (int32 *) ARR_DATA_PTR(blocking_pids_a);
	num_blocking_pids = ArrayGetNItems(ARR_NDIM(blocking_pids_a),
									   ARR_DIMS(blocking_pids_a));

	/*
	 * 检查这些 PID 中是否有任一位于相关 PID 列表中，
	 * 即 isolation tester 正在运行的会话。我们不使用
	 * "arrayoverlaps"，因为这会导致缓存查找，而我们的
	 * 目标之一是在 debug_discard_caches > 0 时快速运行。我们
	 * 预计 blocking_pids 通常是空的，否则在 isolation tester 
	 * 场景中是很小的数字，所以让它充当朴素搜索外层
	 * 循环。
	 */
	for (i = 0; i < num_blocking_pids; i++)
		for (j = 0; j < num_interesting_pids; j++)
		{
			if (blocking_pids[i] == interesting_pids[j])
				PG_RETURN_BOOL(true);
		}

	/*
	 * 检查 blocked_pid 是否正在等待安全快照。理论上
	 * 可以把得到的阻塞者 PID 数组与相关 PID 列表
	 * 对照，但由于 autovacuum 没有
	 * 阻塞 GetSafeSnapshot 的危险，因此花周期分配缓冲区
	 * 并搜索重叠并无多大意义；因此就 isolation tester 的
	 * 用途而言，目前使用单元素缓冲区并检查阻塞者数量
	 * 是否非零就足够了。
	 */
	if (GetSafeSnapshotBlockingPids(blocked_pid, &dummy, 1) > 0)
		PG_RETURN_BOOL(true);

	PG_RETURN_BOOL(false);
}


/*
 * 操作咨询锁的函数
 *
 * 我们如下使用 locktag 字段：
 *
 *	field1: MyDatabaseId ... 确保锁局部于每个数据库
 *	field2: 两个 int4 键之一，或 int8 键高序半部
 *	field3: 第二个 int4 键，或 int8 键低序半部
 *	field4: 用 int8 键时为 1，2 个 int4 键时为 2
 */
#define SET_LOCKTAG_INT64(tag, key64) \
	SET_LOCKTAG_ADVISORY(tag, \
						 MyDatabaseId, \
						 (uint32) ((key64) >> 32), \
						 (uint32) (key64), \
						 1)
#define SET_LOCKTAG_INT32(tag, key1, key2) \
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, key1, key2, 2)

/*
 * pg_advisory_lock(int8) - 获取 int8 键的独占锁
 */
Datum
pg_advisory_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ExclusiveLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock(int8) - 获取事务级 
 * int8 键的独占锁
 */
Datum
pg_advisory_xact_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int8) - 获取 int8 键的共享锁
 */
Datum
pg_advisory_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ShareLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock_shared(int8) - 获取事务级 
 * int8 键的共享锁
 */
Datum
pg_advisory_xact_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;

	SET_LOCKTAG_INT64(tag, key);

	(void) LockAcquire(&tag, ShareLock, false, false);

	PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int8) - 获取 int8 键上的独占锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ExclusiveLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_xact_lock(int8) - 获取事务级 
 * int8 键上的独占锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_xact_lock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ExclusiveLock, false, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_lock_shared(int8) - 获取 int8 键上的共享锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ShareLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_xact_lock_shared(int8) - 获取事务级 
 * int8 键的共享锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_xact_lock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockAcquire(&tag, ShareLock, false, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_advisory_unlock(int8) - 释放 int8 键的独占锁
 *
 * 成功返回 true，未持有锁则返回 false。
*/
Datum
pg_advisory_unlock_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockRelease(&tag, ExclusiveLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int8) - 释放 int8 键的共享锁
 *
 * 成功返回 true，未持有锁则返回 false。
 */
Datum
pg_advisory_unlock_shared_int8(PG_FUNCTION_ARGS)
{
	int64		key = PG_GETARG_INT64(0);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT64(tag, key);

	res = LockRelease(&tag, ShareLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_lock(int4, int4) - 获取 2 个 int4 键的独占锁
 */
Datum
pg_advisory_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ExclusiveLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock(int4, int4) - 获取事务级 
 * 2 个 int4 键的独占锁
 */
Datum
pg_advisory_xact_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ExclusiveLock, false, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_lock_shared(int4, int4) - 获取 2 个 int4 键的共享锁
 */
Datum
pg_advisory_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ShareLock, true, false);

	PG_RETURN_VOID();
}

/*
 * pg_advisory_xact_lock_shared(int4, int4) - 获取事务级 
 * 2 个 int4 键共享锁
 */
Datum
pg_advisory_xact_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;

	SET_LOCKTAG_INT32(tag, key1, key2);

	(void) LockAcquire(&tag, ShareLock, false, false);

	PG_RETURN_VOID();
}

/*
 * pg_try_advisory_lock(int4, int4) - 获取 2 个 int4 键的独占锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ExclusiveLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_xact_lock(int4, int4) - 获取事务级 
 * 2 个 int4 键的独占锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_xact_lock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ExclusiveLock, false, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_lock_shared(int4, int4) - 获取 2 个 int4 键共享锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ShareLock, true, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_try_advisory_xact_lock_shared(int4, int4) - 获取事务级 
 * 2个 int4 键共享锁，不等待
 *
 * 成功则返回 true，锁不可用则返回 false。
 */
Datum
pg_try_advisory_xact_lock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	LockAcquireResult res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockAcquire(&tag, ShareLock, false, true);

	PG_RETURN_BOOL(res != LOCKACQUIRE_NOT_AVAIL);
}

/*
 * pg_advisory_unlock(int4, int4) - 释放 2 个 int4 键的独占锁
 *
 * 成功返回 true，未持有锁则返回 false。
*/
Datum
pg_advisory_unlock_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockRelease(&tag, ExclusiveLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_shared(int4, int4) - 释放 2 个 int4 键的共享锁
 *
 * 成功返回 true，未持有锁则返回 false。
 */
Datum
pg_advisory_unlock_shared_int4(PG_FUNCTION_ARGS)
{
	int32		key1 = PG_GETARG_INT32(0);
	int32		key2 = PG_GETARG_INT32(1);
	LOCKTAG		tag;
	bool		res;

	SET_LOCKTAG_INT32(tag, key1, key2);

	res = LockRelease(&tag, ShareLock, true);

	PG_RETURN_BOOL(res);
}

/*
 * pg_advisory_unlock_all() - 释放所有咨询锁
 */
Datum
pg_advisory_unlock_all(PG_FUNCTION_ARGS)
{
	LockReleaseSession(USER_LOCKMETHOD);

	PG_RETURN_VOID();
}
