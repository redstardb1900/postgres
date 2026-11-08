/*-------------------------------------------------------------------------
 *
 * origin.c
 *	  逻辑复制进度跟踪支持。             
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/logical/origin.c
 *
 * NOTES
 *
 * 本文件提供以下内容：   
 * * 用于在复制环境中命名节点的基础设施   
 * * 一种能以高效且持久的方式存储并持久化复制进度    
 *	 的设施。                 
 *
 * 复制源由一个描述性的、由用户定义的外部名称以及  
 * 一个简短（因此省空间）的内部 2 字节名称组成。这  
 * 种拆分的原因是复制源必须存入 WAL 和共享内存，    
 * 而长描述符会很低效。目前复制源内部 id 只用 2 字节，  
 * 因为一个复制环境中很快出现超过 65k 个节点的可能性    
 * 看起来不大；且只使用两个字节也让我们在空间上       
 * 更高效。                                  
 *
 * 复制进度在一个共享内存表（                 
 * ReplicationState）中跟踪，每次检查点时都会转储到  
 * 条目（"槽"）由内部 id 标识。这是因为这样允许在崩溃  
 * 恢复期间推进复制进度。为此，将事务的原始   
 * LSN（来自源系统）存入提交记录中。从而能在崩溃恢复后
 * 恢复出精确的重放状态；而无需同步提交。允许逻辑    
 * 复制使用异步提交通常对性能有好处，                       
 * 但这尤为重要，因为它允许单线程的重放进程跟上 
 * 拥有多个后端并发产生变更的源端。出于                 
 * 效率以及简单性方面的考虑，一个后端可以                
 * 设置一个复制源，从那时起它就会被用作该               
 * 后端所产生的变更的来源，直到它再次被重                  
 * 置为止。                                                     
 *
 * 该基础架构旨在与逻辑解码配合使用。从远程系统重 
 * 放时，所配置的复制源被提供给输出插件，从而允许 
 * 防止复制循环以及                                                
 * 其他过滤。 
 *
 * 这里有多个层级的锁在起作用：  
 *
 * * 要创建和删除复制源，在整个期间都需要对  
 *	 pg_replication_slot 持有排他锁。这允许我们用脏快照  
 *	 安全且无冲突地分配新的复制源。                      
 *
 * * 创建内存中的复制进度槽时，必须以独占方式持有        
 *	 ReplicationOrigin LWLock；遍历复制进度时则必须持有共    
 *	 享锁；而当推进尚未被设置为会话复制源的         
 *	 某个单独后端的复制进度时，同样也必须持有           
 *	 共享锁。                 
 *
 * * 当操纵或查看某个复制进度槽的 remote_lsn 和               
 *	 local_lsn 字段时必须持有该槽的 lwlock。这主要因为  
 *	 我们不假设 8 字节写入（LSN）在所有平台均为原子，  
 *	 但这也简化远程与本地 lsn 间的内存序问题。       
 *	 我们使用 lwlock 而非 spinlock，这样在 WAL 写入             
 *	 期间持有该锁的危害更小                     
 *	 见 AdvanceReplicationProgress。
 *
 * ---------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/pg_subscription.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "pgstat.h"
#include "replication/logical.h"
#include "replication/origin.h"
#include "storage/condition_variable.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 单个远程节点的重放进度。    
 */
typedef struct ReplicationState
{
	/*
	 * 远程节点的本地标识符。    
	 */
	RepOriginId roident;

	/*
	 * 来自远程侧的最新提交的位置。         
	 */
	XLogRecPtr	remote_lsn;

	/*
	 * 记住提交记录的本地 lsn，以便检查点期间能对它执行 
	 * XLogFlush()，从而知道提交记录确实已安全落盘         
	 * 。  
	 */
	XLogRecPtr	local_lsn;

	/*
	 * 持有该槽的后端的 PID，若无则为 0。  
	 */
	int			acquired_by;

	/*
	 * 当 acquired_by 改变时被通知的条件变量。        
	 */
	ConditionVariable origin_cv;

	/*
	 * 保护 remote_lsn 和 local_lsn 的锁。
	 */
	LWLock		lock;
} ReplicationState;

/*
 * ReplicationState 的磁盘版本。 
 */
typedef struct ReplicationStateOnDisk
{
	RepOriginId roident;
	XLogRecPtr	remote_lsn;
} ReplicationStateOnDisk;


typedef struct ReplicationStateCtl
{
	/* 用于 per-origin LWLock 的 tranche  */
	int			tranche_id;
	/* 长度 max_replication_slots 的数组*/
	ReplicationState states[FLEXIBLE_ARRAY_MEMBER];
} ReplicationStateCtl;

/* 外部变量       */
RepOriginId replorigin_session_origin = InvalidRepOriginId; /* 假定的身份  */
XLogRecPtr	replorigin_session_origin_lsn = InvalidXLogRecPtr;
TimestampTz replorigin_session_origin_timestamp = 0;

/*
 * 指向共享内存中复制状态数组的基址，该数组大小为
 * max_replication_slots.
 *
 * XXX: 是否应该用单独的变量来定其大小而非       
 * max_replication_slots?
 */
static ReplicationState *replication_states;

/*
 * 实际共享内存块（replication_states[] 现为其一部分）。 
 */
static ReplicationStateCtl *replication_states_ctl;

/*
 * 后端本地的缓存元素，取自 ReplicationState，供正在重放 
 * 远程提交的后端使用，这样就无需为了查找后端当前的 
 * RepOriginId 而去搜索它。   
 */
static ReplicationState *session_replication_state = NULL;

/* 磁盘文件的魔数。 */
#define REPLICATION_STATE_MAGIC ((uint32) 0x1257DADE)

static void
replorigin_check_prerequisites(bool check_slots, bool recoveryOK)
{
	if (check_slots && max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot query or manipulate replication origin when max_replication_slots = 0")));

	if (!recoveryOK && RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot manipulate replication origins during recovery")));
}


/*
 * IsReservedOriginName
 *		名称为 "none" 或 "any" 时为真。 
 */
static bool
IsReservedOriginName(const char *name)
{
	return ((pg_strcasecmp(name, LOGICALREP_ORIGIN_NONE) == 0) ||
			(pg_strcasecmp(name, LOGICALREP_ORIGIN_ANY) == 0));
}

/* ---------------------------------------------------------------------------
 * 用于操作复制源本身的函数。                   
 * ---------------------------------------------------------------------------
 */

/*
 * 检查由名称标识的持久化复制源是否存在。    
 *
 * 若节点尚未知且 missing_ok 为 true 则返回 InvalidOid。     
 */
RepOriginId
replorigin_by_name(const char *roname, bool missing_ok)
{
	Form_pg_replication_origin ident;
	Oid			roident = InvalidOid;
	HeapTuple	tuple;
	Datum		roname_d;

	roname_d = CStringGetTextDatum(roname);

	tuple = SearchSysCache1(REPLORIGNAME, roname_d);
	if (HeapTupleIsValid(tuple))
	{
		ident = (Form_pg_replication_origin) GETSTRUCT(tuple);
		roident = ident->roident;
		ReleaseSysCache(tuple);
	}
	else if (!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication origin \"%s\" does not exist",
						roname)));

	return roident;
}

/*
 * 创建一个复制源。    
 *
 * 需要在事务中调用。         
 */
RepOriginId
replorigin_create(const char *roname)
{
	Oid			roident;
	HeapTuple	tuple = NULL;
	Relation	rel;
	Datum		roname_d;
	SnapshotData SnapshotDirty;
	SysScanDesc scan;
	ScanKeyData key;

	roname_d = CStringGetTextDatum(roname);

	Assert(IsTransactionState());

	/*
	 * 我们需要数字复制源宽 16 位，因此无法依赖常规的  
	 * oid 分配方式。作为替代，我们只需扫描     
	 * pg_replication_origin 来找出第一个未使用的 id。这并不   
	 * 是十分高效，但这应当是相当少见的操作；当        
	 * 事实证明它需要更快时，我们大可为此多花一点代   
	 * 码。 
	 *
	 * 我们通过对表持有排他锁（允许读！）处理并发， 
	 * 即在搜索期间一直持有该锁。由于使用"脏快照"，就  
	 * 可读其他进行中会话已写的行，即便它们对常规
	 * 快照不可见。由于有排他锁，因此在我们检查期间     
	 * 并不会有任何新行出现的                                     
	 * 危险。      
	 */
	InitDirtySnapshot(SnapshotDirty);

	rel = table_open(ReplicationOriginRelationId, ExclusiveLock);

	for (roident = InvalidOid + 1; roident < PG_UINT16_MAX; roident++)
	{
		bool		nulls[Natts_pg_replication_origin];
		Datum		values[Natts_pg_replication_origin];
		bool		collides;

		CHECK_FOR_INTERRUPTS();

		ScanKeyInit(&key,
					Anum_pg_replication_origin_roident,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(roident));

		scan = systable_beginscan(rel, ReplicationOriginIdentIndex,
								  true /* indexOK */ ,
								  &SnapshotDirty,
								  1, &key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		if (!collides)
		{
			/*
			 * 好，找到未用的 roident，插入新行并做 CCI，    
			 * 这样调用方若想查找便能查到它。 
			 */
			memset(&nulls, 0, sizeof(nulls));

			values[Anum_pg_replication_origin_roident - 1] = ObjectIdGetDatum(roident);
			values[Anum_pg_replication_origin_roname - 1] = roname_d;

			tuple = heap_form_tuple(RelationGetDescr(rel), values, nulls);
			CatalogTupleInsert(rel, tuple);
			CommandCounterIncrement();
			break;
		}
	}

	/* 现在再释放锁。   */
	table_close(rel, ExclusiveLock);

	if (tuple == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("could not find free replication origin ID")));

	heap_freetuple(tuple);
	return roident;
}

/*
 * 用于删除复制源的辅助函数。      
 */
static void
replorigin_state_clear(RepOriginId roident, bool nowait)
{
	int			i;

	/*
	 * 若存在匹配的槽，则清理该槽的状态信息。   
	 */
restart:
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationState *state = &replication_states[i];

		if (state->roident == roident)
		{
			/* 已找到槽，占用中？ */
			if (state->acquired_by != 0)
			{
				ConditionVariable *cv;

				if (nowait)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_IN_USE),
							 errmsg("could not drop replication origin with ID %d, in use by PID %d",
									state->roident,
									state->acquired_by)));

				/*
				 * 必须等待后重试。由于至此才知道该在哪个 
				 * CV 上等待，因此无法方便地使用 
				 * ConditionVariablePrepareToSleep（在此调用它会是  
				 * 错的，因为那样可能错过信号）；直接使用
				 * ConditionVariableSleep。           
				 */
				cv = &state->origin_cv;

				LWLockRelease(ReplicationOriginLock);

				ConditionVariableSleep(cv, WAIT_EVENT_REPLICATION_ORIGIN_DROP);
				goto restart;
			}

			/* 先写 WAL 日志记录    */
			{
				xl_replorigin_drop xlrec;

				xlrec.node_id = roident;
				XLogBeginInsert();
				XLogRegisterData((char *) (&xlrec), sizeof(xlrec));
				XLogInsert(RM_REPLORIGIN_ID, XLOG_REPLORIGIN_DROP);
			}

			/* 然后清除内存中的槽   */
			state->roident = InvalidRepOriginId;
			state->remote_lsn = InvalidXLogRecPtr;
			state->local_lsn = InvalidXLogRecPtr;
			break;
		}
	}
	LWLockRelease(ReplicationOriginLock);
	ConditionVariableCancelSleep();
}

/*
 * 删除复制源（按名称）。 
 *
 * 需要在事务中调用。         
 */
void
replorigin_drop_by_name(const char *name, bool missing_ok, bool nowait)
{
	RepOriginId roident;
	Relation	rel;
	HeapTuple	tuple;

	Assert(IsTransactionState());

	rel = table_open(ReplicationOriginRelationId, RowExclusiveLock);

	roident = replorigin_by_name(name, missing_ok);

	/* 锁定该复制源以防止并发删除。   */
	LockSharedObject(ReplicationOriginRelationId, roident, 0,
					 AccessExclusiveLock);

	tuple = SearchSysCache1(REPLORIGIDENT, ObjectIdGetDatum(roident));
	if (!HeapTupleIsValid(tuple))
	{
		if (!missing_ok)
			elog(ERROR, "cache lookup failed for replication origin with ID %d",
				 roident);

		/*
		 * 若该复制源已被删除，则我们不需要保留这些锁。 
		 */
		UnlockSharedObject(ReplicationOriginRelationId, roident, 0,
						   AccessExclusiveLock);
		table_close(rel, RowExclusiveLock);
		return;
	}

	replorigin_state_clear(roident, nowait);

	/*
	 * 现在可删除系统表条目了。 
	 */
	CatalogTupleDelete(rel, &tuple->t_self);
	ReleaseSysCache(tuple);

	CommandCounterIncrement();

	/* 提交前一直持有 pg_replication_origin 上的锁  */
	table_close(rel, NoLock);
}

/*
 * 通过其 oid 查找复制源并返回名称。           
 *
 * 外部名称在调用者的上下文中 palloc。    
 *
 * 若复制源已知则返回 true，否则 false。   
 */
bool
replorigin_by_oid(RepOriginId roident, bool missing_ok, char **roname)
{
	HeapTuple	tuple;
	Form_pg_replication_origin ric;

	Assert(OidIsValid((Oid) roident));
	Assert(roident != InvalidRepOriginId);
	Assert(roident != DoNotReplicateId);

	tuple = SearchSysCache1(REPLORIGIDENT,
							ObjectIdGetDatum((Oid) roident));

	if (HeapTupleIsValid(tuple))
	{
		ric = (Form_pg_replication_origin) GETSTRUCT(tuple);
		*roname = text_to_cstring(&ric->roname);
		ReleaseSysCache(tuple);

		return true;
	}
	else
	{
		*roname = NULL;

		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("replication origin with ID %d does not exist",
							roident)));

		return false;
	}
}


/* ---------------------------------------------------------------------------
 * 用于处理复制进度的函数。        
 * ---------------------------------------------------------------------------
 */

Size
ReplicationOriginShmemSize(void)
{
	Size		size = 0;

	/*
	 * XXX: 使用 max_replication_slots 未必恰当，因为这里保存   
	 * 的是 *远程* 事务的重放状态。但目前重用它似乎已经 
	 * 足够，而不是引入一个单独的 GUC。               
	 */
	if (max_replication_slots == 0)
		return size;

	size = add_size(size, offsetof(ReplicationStateCtl, states));

	size = add_size(size,
					mul_size(max_replication_slots, sizeof(ReplicationState)));
	return size;
}

void
ReplicationOriginShmemInit(void)
{
	bool		found;

	if (max_replication_slots == 0)
		return;

	replication_states_ctl = (ReplicationStateCtl *)
		ShmemInitStruct("ReplicationOriginState",
						ReplicationOriginShmemSize(),
						&found);
	replication_states = replication_states_ctl->states;

	if (!found)
	{
		int			i;

		MemSet(replication_states_ctl, 0, ReplicationOriginShmemSize());

		replication_states_ctl->tranche_id = LWTRANCHE_REPLICATION_ORIGIN_STATE;

		for (i = 0; i < max_replication_slots; i++)
		{
			LWLockInitialize(&replication_states[i].lock,
							 replication_states_ctl->tranche_id);
			ConditionVariableInit(&replication_states[i].origin_cv);
		}
	}
}

/* ---------------------------------------------------------------------------
 * 针对重放到的 remote_lsn，对每个复制源的进度执行一次   
 * 检查点。务必确保检查点（local_lsn）中引用的所有事务   
 * 确实都已经写到磁盘上。如果这些事务最初是以异         
 * 步方式提交，那么情况可能还不是这样。       
 *
 * 我们以下列格式存储检查点：      
 * +-------+------------------------+------------------+-----+--------+
 * | MAGIC | ReplicationStateOnDisk | struct Replic... | ... | CRC32C | EOF
 * +-------+------------------------+------------------+-----+--------+
 *
 * 所以它就是 magic 加上后面静态定长的      
 * ReplicationStateOnDisk 结构。注意 ReplicationState 的    
 * 最大数量由 max_replication_slots 决定。         
 * ---------------------------------------------------------------------------
 */
void
CheckPointReplicationOrigin(void)
{
	const char *tmppath = "pg_logical/replorigin_checkpoint.tmp";
	const char *path = "pg_logical/replorigin_checkpoint";
	int			tmpfd;
	int			i;
	uint32		magic = REPLICATION_STATE_MAGIC;
	pg_crc32c	crc;

	if (max_replication_slots == 0)
		return;

	INIT_CRC32C(crc);

	/* 确保没有遗留的旧临时文件    */
	if (unlink(tmppath) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						tmppath)));

	/*
	 * 没有其他后端能在同一时间执行它；一次只能发生一  
	 * 个检查点。      
	 */
	tmpfd = OpenTransientFile(tmppath,
							  O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
	if (tmpfd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));

	/* 写入魔数*/
	errno = 0;
	if ((write(tmpfd, &magic, sizeof(magic))) != sizeof(magic))
	{
		/* 若写入未设置 errno，则假定是磁盘空间不足  */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
	}
	COMP_CRC32C(crc, &magic, sizeof(magic));

	/* 防止并发创建/删除。       */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	/* 写实际数据   */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationStateOnDisk disk_state;
		ReplicationState *curstate = &replication_states[i];
		XLogRecPtr	local_lsn;

		if (curstate->roident == InvalidRepOriginId)
			continue;

		/* 置零，避免未初始化的填充字节 */
		memset(&disk_state, 0, sizeof(disk_state));

		LWLockAcquire(&curstate->lock, LW_SHARED);

		disk_state.roident = curstate->roident;

		disk_state.remote_lsn = curstate->remote_lsn;
		local_lsn = curstate->local_lsn;

		LWLockRelease(&curstate->lock);

		/* 确保我们只写出持久化的提交记录          */
		XLogFlush(local_lsn);

		errno = 0;
		if ((write(tmpfd, &disk_state, sizeof(disk_state))) !=
			sizeof(disk_state))
		{
			/* 若写入未设置 errno，则假定是磁盘空间不足  */
			if (errno == 0)
				errno = ENOSPC;
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							tmppath)));
		}

		COMP_CRC32C(crc, &disk_state, sizeof(disk_state));
	}

	LWLockRelease(ReplicationOriginLock);

	/* 写出 CRC        */
	FIN_CRC32C(crc);
	errno = 0;
	if ((write(tmpfd, &crc, sizeof(crc))) != sizeof(crc))
	{
		/* 若写入未设置 errno，则假定是磁盘空间不足  */
		if (errno == 0)
			errno = ENOSPC;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
	}

	if (CloseTransientFile(tmpfd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						tmppath)));

	/* fsync，重命名为永久文件，fsync 文件和目录  */
	durable_rename(tmppath, path, PANIC);
}

/*
 * 从早前由 CheckPointReplicationOrigin 保存的检查点数据      
 * 中恢复重放状态。    
 *
 * 该函数只需要在启动时调用一次，而 *不是* 在其后恢复
 * 期间的每次检查点读取（例如 HS 或从基础备份进行         
 * PITR）。此后所有状态都能由提交记录恢复。     
 */
void
StartupReplicationOrigin(void)
{
	const char *path = "pg_logical/replorigin_checkpoint";
	int			fd;
	int			readBytes;
	uint32		magic = REPLICATION_STATE_MAGIC;
	int			last_state = 0;
	pg_crc32c	file_crc;
	pg_crc32c	crc;

	/* 不希望覆盖已经存在的状态           */
#ifdef USE_ASSERT_CHECKING
	static bool already_started = false;

	Assert(!already_started);
	already_started = true;
#endif

	if (max_replication_slots == 0)
		return;

	INIT_CRC32C(crc);

	elog(DEBUG2, "starting up replication origin progress state");

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);

	/*
	 * 上次运行时 max_replication_slots 可能为 0，或者刚启动一
	 * 台备库。 
	 */
	if (fd < 0 && errno == ENOENT)
		return;
	else if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m",
						path)));

	/* 校验魔数，即使没有任何活动也会写入它   */
	readBytes = read(fd, &magic, sizeof(magic));
	if (readBytes != sizeof(magic))
	{
		if (readBytes < 0)
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		else
			ereport(PANIC,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, sizeof(magic))));
	}
	COMP_CRC32C(crc, &magic, sizeof(magic));

	if (magic != REPLICATION_STATE_MAGIC)
		ereport(PANIC,
				(errmsg("replication checkpoint has wrong magic %u instead of %u",
						magic, REPLICATION_STATE_MAGIC)));

	/* 这里可以跳过加锁，不可能有其他访问   */

	/* 恢复各个单独的状态，直到找不到更多为止      */
	while (true)
	{
		ReplicationStateOnDisk disk_state;

		readBytes = read(fd, &disk_state, sizeof(disk_state));

		/* 无更多数据 */
		if (readBytes == sizeof(crc))
		{
			/* 不美观，但简单。   */
			file_crc = *(pg_crc32c *) &disk_state;
			break;
		}

		if (readBytes < 0)
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m",
							path)));
		}

		if (readBytes != sizeof(disk_state))
		{
			ereport(PANIC,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": read %d of %zu",
							path, readBytes, sizeof(disk_state))));
		}

		COMP_CRC32C(crc, &disk_state, sizeof(disk_state));

		if (last_state == max_replication_slots)
			ereport(PANIC,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("could not find free replication state, increase max_replication_slots")));

		/* 拷贝到共享内存      */
		replication_states[last_state].roident = disk_state.roident;
		replication_states[last_state].remote_lsn = disk_state.remote_lsn;
		last_state++;

		ereport(LOG,
				(errmsg("recovered replication state of node %d to %X/%X",
						disk_state.roident,
						LSN_FORMAT_ARGS(disk_state.remote_lsn))));
	}

	/* 再检查校验和 */
	FIN_CRC32C(crc);
	if (file_crc != crc)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("replication slot checkpoint has wrong checksum %u, expected %u",
						crc, file_crc)));

	if (CloseTransientFile(fd) != 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						path)));
}

void
replorigin_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_REPLORIGIN_SET:
			{
				xl_replorigin_set *xlrec =
					(xl_replorigin_set *) XLogRecGetData(record);

				replorigin_advance(xlrec->node_id,
								   xlrec->remote_lsn, record->EndRecPtr,
								   xlrec->force /* 向后   */ ,
								   false /* WAL log */ );
				break;
			}
		case XLOG_REPLORIGIN_DROP:
			{
				xl_replorigin_drop *xlrec;
				int			i;

				xlrec = (xl_replorigin_drop *) XLogRecGetData(record);

				for (i = 0; i < max_replication_slots; i++)
				{
					ReplicationState *state = &replication_states[i];

					/* 已找到槽   */
					if (state->roident == xlrec->node_id)
					{
						/* 重置条目*/
						state->roident = InvalidRepOriginId;
						state->remote_lsn = InvalidXLogRecPtr;
						state->local_lsn = InvalidXLogRecPtr;
						break;
					}
				}
				break;
			}
		default:
			elog(PANIC, "replorigin_redo: unknown op code %u", info);
	}
}


/*
 * 告诉复制源进度机制，来自 'node' 的某个提交已在远程 
 * 节点上以 remote_commit LSN 产生，并且已被成功重放，     
 * 我们无需再次对其进行重放。与设置                       
 * replorigin_session_origin_lsn 和 replorigin_session_origin           
 * 一起可确保崩溃后我们不会丢失相关信息，前提     
 * 是该事务产生了持久效果（想想异步提交）。        
 *
 * local_commit 需要是该提交的本地 LSN，以便我们能确保       
 * 在检查点时有足够的 WAL 已持久化到磁盘。     
 *
 * 调用时需要在 pg_replication_origin 上持有 RowExclusiveLock 
 * 锁，除非恢复中。   
 */
void
replorigin_advance(RepOriginId node,
				   XLogRecPtr remote_commit, XLogRecPtr local_commit,
				   bool go_backward, bool wal_log)
{
	int			i;
	ReplicationState *replication_state = NULL;
	ReplicationState *free_state = NULL;

	Assert(node != InvalidRepOriginId);

	/* 不跟踪 DoNotReplicateId      */
	if (node == DoNotReplicateId)
		return;

	/*
	 * XXX: 若由 WAL 重放调用，更高效的做法是先恢复进  
	 * 后端本地的哈希表，待恢复完成后再转储到共享内存 
	 * 中。等到能证明这是可测量的开销时，我们再来    
	 * 实现它。                              
	 */

	/* 独占加锁，因为可能必须创建新的表条目。     */
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	/*
	 * 查找该复制源现有的槽，或者一个我们能使用的空闲  
	 * 槽 
	 */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationState *curstate = &replication_states[i];

		/* 记住必要时插入的位置        */
		if (curstate->roident == InvalidRepOriginId &&
			free_state == NULL)
		{
			free_state = curstate;
			continue;
		}

		/* 槽不匹配 */
		if (curstate->roident != node)
		{
			continue;
		}

		/* 找到槽了   */
		replication_state = curstate;

		LWLockAcquire(&replication_state->lock, LW_EXCLUSIVE);

		/* 确保它没有被其他任何人使用  */
		if (replication_state->acquired_by != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication origin with ID %d is already active for PID %d",
							replication_state->roident,
							replication_state->acquired_by)));
		}

		break;
	}

	if (replication_state == NULL && free_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not find free replication state slot for replication origin with ID %d",
						node),
				 errhint("Increase max_replication_slots and try again.")));

	if (replication_state == NULL)
	{
		/* 初始化新槽     */
		LWLockAcquire(&free_state->lock, LW_EXCLUSIVE);
		replication_state = free_state;
		Assert(replication_state->remote_lsn == InvalidXLogRecPtr);
		Assert(replication_state->local_lsn == InvalidXLogRecPtr);
		replication_state->roident = node;
	}

	Assert(replication_state->roident != InvalidRepOriginId);

	/*
	 * 如果有人"强制"设置此槽，就为其写 WAL 日志，使其 
	 * 具有持久性且备库能得到消息。这主要会在 WAL 重放  
	 * （提交记录重放）中调用，此时无需写 WAL 日志。
	 */
	if (wal_log)
	{
		xl_replorigin_set xlrec;

		xlrec.remote_lsn = remote_commit;
		xlrec.node_id = node;
		xlrec.force = go_backward;

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), sizeof(xlrec));

		XLogInsert(RM_REPLORIGIN_ID, XLOG_REPLORIGIN_SET);
	}

	/*
	 * 由于检查点期间的无害竞态条件，我们可能在这里  
	 * 看到比内存中已有的值更旧的值。对已准备事务，当 
	 * prepare 稍后才连同 commit prepared 一起发送，并且在      
	 * prepare 与 commit prepared 之间存在其他事务的提交。请  
	 * 参见 ReorderBufferFinishPrepared。                                  
	 * 不要覆盖这些值。                           
	 */
	if (go_backward || replication_state->remote_lsn < remote_commit)
		replication_state->remote_lsn = remote_commit;
	if (local_commit != InvalidXLogRecPtr &&
		(go_backward || replication_state->local_lsn < local_commit))
		replication_state->local_lsn = local_commit;
	LWLockRelease(&replication_state->lock);

	/*
	 * 在更改 LSN *之后*才释放；该槽未被获取，否则随时  
	 * 可能被删除。           
	 */
	LWLockRelease(ReplicationOriginLock);
}


XLogRecPtr
replorigin_get_progress(RepOriginId node, bool flush)
{
	int			i;
	XLogRecPtr	local_lsn = InvalidXLogRecPtr;
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;

	/* 防止槽被并发地删除。                */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationState *state;

		state = &replication_states[i];

		if (state->roident == node)
		{
			LWLockAcquire(&state->lock, LW_SHARED);

			remote_lsn = state->remote_lsn;
			local_lsn = state->local_lsn;

			LWLockRelease(&state->lock);

			break;
		}
	}

	LWLockRelease(ReplicationOriginLock);

	if (flush && local_lsn != InvalidXLogRecPtr)
		XLogFlush(local_lsn);

	return remote_lsn;
}

/*
 * 在进程退出期间拆除一个（可能）已配置的会话复制源   
 * 。  
 */
static void
ReplicationOriginExitCleanup(int code, Datum arg)
{
	ConditionVariable *cv = NULL;

	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	if (session_replication_state != NULL &&
		session_replication_state->acquired_by == MyProcPid)
	{
		cv = &session_replication_state->origin_cv;

		session_replication_state->acquired_by = 0;
		session_replication_state = NULL;
	}

	LWLockRelease(ReplicationOriginLock);

	if (cv)
		ConditionVariableBroadcast(cv);
}

/*
 * 若共享内存结构中尚无此复制源，在其中设置一个  
 * 并缓存对特定 ReplicationSlot 的访问，这样调用          
 * replorigin_session_advance() 时就           
 * 不必搜索该数组。     
 *
 * 通常每个进程只能存在一个这样被缓存的复制源，因   
 * 此缓存值只有在前一个值通过                               
 * replorigin_session_reset() 拆除后才能再次设置。对于这   
 * 种常规情况，传递 acquired_by = 0（含义是该槽不允许         
 *
 * 已被其他进程获取）。不过，有时多个进程可以安全地    
 * 重用同一个复制源槽（例如多个并行应用进程可以安  
 * 全使用同一个复制源，只要它们通过一次只允许一个      
 * 进程提交来维持提交顺序）。这种情况下第一个进程     
 * 必须传 acquired_by = 0，随后共享同一源的其他进程 
 * 能传 acquired_by = 首进程 PID。  
 */
void
replorigin_session_setup(RepOriginId node, int acquired_by)
{
	static bool registered_cleanup;
	int			i;
	int			free_slot = -1;

	if (!registered_cleanup)
	{
		on_shmem_exit(ReplicationOriginExitCleanup, 0);
		registered_cleanup = true;
	}

	Assert(max_replication_slots > 0);

	if (session_replication_state != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot setup replication origin when one is already setup")));

	/* 独占加锁，因为可能必须创建新的表条目。     */
	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	/*
	 * 查找该复制源现有的槽，或者一个我们能使用的空闲  
	 * 槽 
	 */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationState *curstate = &replication_states[i];

		/* 记住必要时插入的位置        */
		if (curstate->roident == InvalidRepOriginId &&
			free_slot == -1)
		{
			free_slot = i;
			continue;
		}

		/* 槽不匹配 */
		if (curstate->roident != node)
			continue;

		else if (curstate->acquired_by != 0 && acquired_by == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_IN_USE),
					 errmsg("replication origin with ID %d is already active for PID %d",
							curstate->roident, curstate->acquired_by)));
		}

		/* 找到槽了   */
		session_replication_state = curstate;
	}


	if (session_replication_state == NULL && free_slot == -1)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("could not find free replication state slot for replication origin with ID %d",
						node),
				 errhint("Increase max_replication_slots and try again.")));
	else if (session_replication_state == NULL)
	{
		/* 初始化新槽     */
		session_replication_state = &replication_states[free_slot];
		Assert(session_replication_state->remote_lsn == InvalidXLogRecPtr);
		Assert(session_replication_state->local_lsn == InvalidXLogRecPtr);
		session_replication_state->roident = node;
	}


	Assert(session_replication_state->roident != InvalidRepOriginId);

	if (acquired_by == 0)
		session_replication_state->acquired_by = MyProcPid;
	else if (session_replication_state->acquired_by != acquired_by)
		elog(ERROR, "could not find replication state slot for replication origin with OID %u which was acquired by %d",
			 node, acquired_by);

	LWLockRelease(ReplicationOriginLock);

	/* 可能这一条并无意义    */
	ConditionVariableBroadcast(&session_replication_state->origin_cv);
}

/*
 * 重置本会话此前设置的重放状态。       
 *
 * 仅当已用 replorigin_session_setup() 设置过复制源  
 * 时才能调用本函数。
 */
void
replorigin_session_reset(void)
{
	ConditionVariable *cv;

	Assert(max_replication_slots != 0);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	LWLockAcquire(ReplicationOriginLock, LW_EXCLUSIVE);

	session_replication_state->acquired_by = 0;
	cv = &session_replication_state->origin_cv;
	session_replication_state = NULL;

	LWLockRelease(ReplicationOriginLock);

	ConditionVariableBroadcast(cv);
}

/*
 * 做 replorigin_advance() 的同一工作，只是作用于会话 
 * 的复制源。   
 *
 * 这比使用 replorigin_advance() 明显更便宜。       
 */
void
replorigin_session_advance(XLogRecPtr remote_commit, XLogRecPtr local_commit)
{
	Assert(session_replication_state != NULL);
	Assert(session_replication_state->roident != InvalidRepOriginId);

	LWLockAcquire(&session_replication_state->lock, LW_EXCLUSIVE);
	if (session_replication_state->local_lsn < local_commit)
		session_replication_state->local_lsn = local_commit;
	if (session_replication_state->remote_lsn < remote_commit)
		session_replication_state->remote_lsn = remote_commit;
	LWLockRelease(&session_replication_state->lock);
}

/*
 * 向该机制询问我们已经成功重放了来自某个已设置复 
 * 制源的变更到哪一点。                   
 */
XLogRecPtr
replorigin_session_get_progress(bool flush)
{
	XLogRecPtr	remote_lsn;
	XLogRecPtr	local_lsn;

	Assert(session_replication_state != NULL);

	LWLockAcquire(&session_replication_state->lock, LW_SHARED);
	remote_lsn = session_replication_state->remote_lsn;
	local_lsn = session_replication_state->local_lsn;
	LWLockRelease(&session_replication_state->lock);

	if (flush && local_lsn != InvalidXLogRecPtr)
		XLogFlush(local_lsn);

	return remote_lsn;
}



/* ---------------------------------------------------------------------------
 * 用于操作复制源的 SQL 函数。            
 *
 * 这些大多应当是围绕更通用函数的相当简短的包装器。   
 * ---------------------------------------------------------------------------
 */

/*
 * 为传入的名称创建复制源，并返回所分配的                
 * oid.
 */
Datum
pg_replication_origin_create(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId roident;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));

	/*
	 * 复制源 "any" 和 "none" 被保留用于系统选项。           
	 * 复制源 "pg_xxx" 被保留供内部使用。     
	 */
	if (IsReservedName(name) || IsReservedOriginName(name))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("replication origin name \"%s\" is reserved",
						name),
				 errdetail("Origin names \"%s\", \"%s\", and names starting with \"pg_\" are reserved.",
						   LOGICALREP_ORIGIN_ANY, LOGICALREP_ORIGIN_NONE)));

	/*
	 * 若以适当开关编译，则当复制源名称违反回归   
	 * 测试约定时就会抱怨。                        
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(name, "regress_", 8) != 0)
		elog(WARNING, "replication origins created by regression test cases should have names starting with \"regress_\"");
#endif

	roident = replorigin_create(name);

	pfree(name);

	PG_RETURN_OID(roident);
}

/*
 * 删除复制源。      
 */
Datum
pg_replication_origin_drop(PG_FUNCTION_ARGS)
{
	char	   *name;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));

	replorigin_drop_by_name(name, false, true);

	pfree(name);

	PG_RETURN_VOID();
}

/*
 * 返回复制源的 oid。          
 */
Datum
pg_replication_origin_oid(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId roident;

	replorigin_check_prerequisites(false, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	roident = replorigin_by_name(name, true);

	pfree(name);

	if (OidIsValid(roident))
		PG_RETURN_OID(roident);
	PG_RETURN_NULL();
}

/*
 * 为当前会话设置一个复制源。     
 */
Datum
pg_replication_origin_session_setup(PG_FUNCTION_ARGS)
{
	char	   *name;
	RepOriginId origin;

	replorigin_check_prerequisites(true, false);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	origin = replorigin_by_name(name, false);
	replorigin_session_setup(origin, 0);

	replorigin_session_origin = origin;

	pfree(name);

	PG_RETURN_VOID();
}

/*
 * 重置本会话中此前设置的复制源。
 */
Datum
pg_replication_origin_session_reset(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(true, false);

	replorigin_session_reset();

	replorigin_session_origin = InvalidRepOriginId;
	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;

	PG_RETURN_VOID();
}

/*
 * 是否已为当前会话设置了复制源。        
 */
Datum
pg_replication_origin_session_is_setup(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(false, false);

	PG_RETURN_BOOL(replorigin_session_origin != InvalidRepOriginId);
}


/*
 * 返回为当前会话设置的复制源的复制进度。               
 *
 * 如果 'flush' 被设置为 true，则能确保返回值对应的本地    
 * 事务已被刷写。这在重放被复制的事务时使用异步提交    
 * 的情况下会很有用。                             
 */
Datum
pg_replication_origin_session_progress(PG_FUNCTION_ARGS)
{
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;
	bool		flush = PG_GETARG_BOOL(0);

	replorigin_check_prerequisites(true, false);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	remote_lsn = replorigin_session_get_progress(flush);

	if (remote_lsn == InvalidXLogRecPtr)
		PG_RETURN_NULL();

	PG_RETURN_LSN(remote_lsn);
}

Datum
pg_replication_origin_xact_setup(PG_FUNCTION_ARGS)
{
	XLogRecPtr	location = PG_GETARG_LSN(0);

	replorigin_check_prerequisites(true, false);

	if (session_replication_state == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("no replication origin is configured")));

	replorigin_session_origin_lsn = location;
	replorigin_session_origin_timestamp = PG_GETARG_TIMESTAMPTZ(1);

	PG_RETURN_VOID();
}

Datum
pg_replication_origin_xact_reset(PG_FUNCTION_ARGS)
{
	replorigin_check_prerequisites(true, false);

	replorigin_session_origin_lsn = InvalidXLogRecPtr;
	replorigin_session_origin_timestamp = 0;

	PG_RETURN_VOID();
}


Datum
pg_replication_origin_advance(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	XLogRecPtr	remote_commit = PG_GETARG_LSN(1);
	RepOriginId node;

	replorigin_check_prerequisites(true, false);

	/* 加锁以防止该复制源消失                     */
	LockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

	node = replorigin_by_name(text_to_cstring(name), false);

	/*
	 * 无法合理地传入要在检查点时刷写的本地提交，因为
	 * 该事务尚未提交。这正是应当用本函数来设置初始复 
	 * 制状态的原因，而不是用于重放。            
	 */
	replorigin_advance(node, remote_commit, InvalidXLogRecPtr,
					   true /* 向后退   */ , true /* WAL log */ );

	UnlockRelationOid(ReplicationOriginRelationId, RowExclusiveLock);

	PG_RETURN_VOID();
}


/*
 * 返回单个复制源的复制进度。                              
 *
 * 如果 'flush' 被设置为 true，则能确保返回值对应的本地    
 * 事务已被刷写。这在重放被复制的事务时使用异步提交    
 * 的情况下会很有用。                             
 */
Datum
pg_replication_origin_progress(PG_FUNCTION_ARGS)
{
	char	   *name;
	bool		flush;
	RepOriginId roident;
	XLogRecPtr	remote_lsn = InvalidXLogRecPtr;

	replorigin_check_prerequisites(true, true);

	name = text_to_cstring((text *) DatumGetPointer(PG_GETARG_DATUM(0)));
	flush = PG_GETARG_BOOL(1);

	roident = replorigin_by_name(name, false);
	Assert(OidIsValid(roident));

	remote_lsn = replorigin_get_progress(roident, flush);

	if (remote_lsn == InvalidXLogRecPtr)
		PG_RETURN_NULL();

	PG_RETURN_LSN(remote_lsn);
}


Datum
pg_show_replication_origin_status(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;
#define REPLICATION_ORIGIN_PROGRESS_COLS 4

	/* 若槽被设置为零，我们希望返回 0 行*/
	replorigin_check_prerequisites(false, true);

	InitMaterializedSRF(fcinfo, 0);

	/* 防止槽被并发地删除。                */
	LWLockAcquire(ReplicationOriginLock, LW_SHARED);

	/*
	 * 遍历全部可能的 replication_states，若已被填充则显示 
	 * 出来。注意我们不加锁，因此略微损坏/过时的值     
	 * 是可能出现的。            
	 */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationState *state;
		Datum		values[REPLICATION_ORIGIN_PROGRESS_COLS];
		bool		nulls[REPLICATION_ORIGIN_PROGRESS_COLS];
		char	   *roname;

		state = &replication_states[i];

		/* 槽未使用，无内容显示  */
		if (state->roident == InvalidRepOriginId)
			continue;

		memset(values, 0, sizeof(values));
		memset(nulls, 1, sizeof(nulls));

		values[0] = ObjectIdGetDatum(state->roident);
		nulls[0] = false;

		/*
		 * 我们并不阻止该复制源被并发删除，因此        
		 * 静默地接受它可能已消失。  
		 */
		if (replorigin_by_oid(state->roident, true,
							  &roname))
		{
			values[1] = CStringGetTextDatum(roname);
			nulls[1] = false;
		}

		LWLockAcquire(&state->lock, LW_SHARED);

		values[2] = LSNGetDatum(state->remote_lsn);
		nulls[2] = false;

		values[3] = LSNGetDatum(state->local_lsn);
		nulls[3] = false;

		LWLockRelease(&state->lock);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	LWLockRelease(ReplicationOriginLock);

#undef REPLICATION_ORIGIN_PROGRESS_COLS

	return (Datum) 0;
}
