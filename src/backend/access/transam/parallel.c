/*-------------------------------------------------------------------------
 *
 * parallel.c
 *	  用于启动并行工作进程的基础设施
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/parallel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/session.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_enum.h"
#include "catalog/storage.h"
#include "commands/async.h"
#include "commands/vacuum.h"
#include "executor/execParallel.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/predicate.h"
#include "storage/sinval.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/combocid.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

/*
 * 我们不想在错误队列上浪费大量内存；多数时候它只处理
 * 少量小消息。不过最好把它做得足够大，以便典型的
 * ErrorResponse 可以无阻塞地发送。这样，出错的工作进程
 * 就能把整条消息写入队列并终止，而不必等待用户
 * 后端。
 */
#define PARALLEL_ERROR_QUEUE_SIZE			16384

/* 并行上下文 TOC 的魔数。 */
#define PARALLEL_MAGIC						0x50477c7c

/*
 * 按上下文划分的并行状态共享所用的魔数。高层代码
 * 应使用较小的值，把这些非常大的值留给本
 * 模块使用。
 */
#define PARALLEL_KEY_FIXED					UINT64CONST(0xFFFFFFFFFFFF0001)
#define PARALLEL_KEY_ERROR_QUEUE			UINT64CONST(0xFFFFFFFFFFFF0002)
#define PARALLEL_KEY_LIBRARY				UINT64CONST(0xFFFFFFFFFFFF0003)
#define PARALLEL_KEY_GUC					UINT64CONST(0xFFFFFFFFFFFF0004)
#define PARALLEL_KEY_COMBO_CID				UINT64CONST(0xFFFFFFFFFFFF0005)
#define PARALLEL_KEY_TRANSACTION_SNAPSHOT	UINT64CONST(0xFFFFFFFFFFFF0006)
#define PARALLEL_KEY_ACTIVE_SNAPSHOT		UINT64CONST(0xFFFFFFFFFFFF0007)
#define PARALLEL_KEY_TRANSACTION_STATE		UINT64CONST(0xFFFFFFFFFFFF0008)
#define PARALLEL_KEY_ENTRYPOINT				UINT64CONST(0xFFFFFFFFFFFF0009)
#define PARALLEL_KEY_SESSION_DSM			UINT64CONST(0xFFFFFFFFFFFF000A)
#define PARALLEL_KEY_PENDING_SYNCS			UINT64CONST(0xFFFFFFFFFFFF000B)
#define PARALLEL_KEY_REINDEX_STATE			UINT64CONST(0xFFFFFFFFFFFF000C)
#define PARALLEL_KEY_RELMAPPER_STATE		UINT64CONST(0xFFFFFFFFFFFF000D)
#define PARALLEL_KEY_UNCOMMITTEDENUMS		UINT64CONST(0xFFFFFFFFFFFF000E)
#define PARALLEL_KEY_CLIENTCONNINFO			UINT64CONST(0xFFFFFFFFFFFF000F)

/* 固定大小的并行状态。 */
typedef struct FixedParallelState
{
	/* 工作进程必须恢复的固定大小状态。 */
	Oid			database_id;
	Oid			authenticated_user_id;
	Oid			session_user_id;
	Oid			outer_user_id;
	Oid			current_user_id;
	Oid			temp_namespace_id;
	Oid			temp_toast_namespace_id;
	int			sec_context;
	bool		authenticated_user_is_superuser;
	bool		session_user_is_superuser;
	bool		role_is_superuser;
	PGPROC	   *parallel_leader_pgproc;
	pid_t		parallel_leader_pid;
	BackendId	parallel_leader_backend_id;
	TimestampTz xact_ts;
	TimestampTz stmt_ts;
	SerializableXactHandle serializable_xact_handle;

	/* 互斥锁保护其余字段。 */
	slock_t		mutex;

	/* 任一工作进程的最大 XactLastRecEnd。 */
	XLogRecPtr	last_xlog_end;
} FixedParallelState;

/*
 * 我们的并行工作进程编号。初始化为 -1，表示我们不是
 * 并行工作进程。在并行工作进程中，会在调用任何用户代码之前
 * 设为 >= 0 且 < 工作进程数的值；每个并行
 * 工作进程会得到不同的并行工作进程编号。
 */
int			ParallelWorkerNumber = -1;

/* 是否有待接收的并行消息？ */
volatile sig_atomic_t ParallelMessagePending = false;

/* 我们是否正在初始化并行工作进程？ */
bool		InitializingParallelWorker = false;

/* 指向我们的固定并行状态的指针。 */
static FixedParallelState *MyFixedParallelState;

/* 活动并行上下文的列表。 */
static dlist_head pcxt_list = DLIST_STATIC_INIT(pcxt_list);

/* FixedParallelState 数据的后端本地副本。 */
static pid_t ParallelLeaderPid;

/*
 * 内部并行工作进程入口点列表。需要它的原因见下方
 * LookupParallelWorkerFunction() 的说明。
 */
static const struct
{
	const char *fn_name;
	parallel_worker_main_type fn_addr;
}			InternalParallelWorkers[] =

{
	{
		"ParallelQueryMain", ParallelQueryMain
	},
	{
		"_bt_parallel_build_main", _bt_parallel_build_main
	},
	{
		"parallel_vacuum_main", parallel_vacuum_main
	}
};

/* 私有函数。 */
static void HandleParallelMessage(ParallelContext *pcxt, int i, StringInfo msg);
static void WaitForParallelWorkersToExit(ParallelContext *pcxt);
static parallel_worker_main_type LookupParallelWorkerFunction(const char *libraryname, const char *funcname);
static void ParallelWorkerShutdown(int code, Datum arg);


/*
 * 建立新的并行上下文。应在进入并行模式之后完成，并且
 * （除非出错）应在退出当前子事务之前销毁该
 * 上下文。
 */
ParallelContext *
CreateParallelContext(const char *library_name, const char *function_name,
					  int nworkers)
{
	MemoryContext oldcontext;
	ParallelContext *pcxt;

	/* 若不在并行模式中创建并行上下文是不安全的。 */
	Assert(IsInParallelMode());

	/* 工作进程数应为非负。 */
	Assert(nworkers >= 0);

	/* 我们可能运行在短寿命的内存上下文中。 */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* 初始化一个新的 ParallelContext。 */
	pcxt = palloc0(sizeof(ParallelContext));
	pcxt->subid = GetCurrentSubTransactionId();
	pcxt->nworkers = nworkers;
	pcxt->nworkers_to_launch = nworkers;
	pcxt->library_name = pstrdup(library_name);
	pcxt->function_name = pstrdup(function_name);
	pcxt->error_context_stack = error_context_stack;
	shm_toc_initialize_estimator(&pcxt->estimator);
	dlist_push_head(&pcxt_list, &pcxt->node);

	/* 恢复先前的内存上下文。 */
	MemoryContextSwitchTo(oldcontext);

	return pcxt;
}

/*
 * 为并行上下文建立动态共享内存段，并把工作进程
 * 所需的状态及其他簿记信息复制
 * 进去。
 */
void
InitializeParallelDSM(ParallelContext *pcxt)
{
	MemoryContext oldcontext;
	Size		library_len = 0;
	Size		guc_len = 0;
	Size		combocidlen = 0;
	Size		tsnaplen = 0;
	Size		asnaplen = 0;
	Size		tstatelen = 0;
	Size		pendingsyncslen = 0;
	Size		reindexlen = 0;
	Size		relmapperlen = 0;
	Size		uncommittedenumslen = 0;
	Size		clientconninfolen = 0;
	Size		segsize = 0;
	int			i;
	FixedParallelState *fps;
	dsm_handle	session_dsm_handle = DSM_HANDLE_INVALID;
	Snapshot	transaction_snapshot = GetTransactionSnapshot();
	Snapshot	active_snapshot = GetActiveSnapshot();

	/* 我们可能运行在非常短寿命的内存上下文中。 */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* 为存放固定大小的并行状态预留空间。 */
	shm_toc_estimate_chunk(&pcxt->estimator, sizeof(FixedParallelState));
	shm_toc_estimate_keys(&pcxt->estimator, 1);

	/*
	 * 若我们在不可中断时到达这里，启动任何工作进程都不安全：
	 * 我们将无法处理它们发来的中断。处理这种边界情况的
	 * 办法是假装没有请求任何
	 * 工作进程。
	 */
	if (!INTERRUPTS_CAN_BE_PROCESSED())
		pcxt->nworkers = 0;

	/*
	 * 通常用户会请求至少一个工作进程，但若碰巧没有，
	 * 这里可以跳过许多步骤。
	 */
	if (pcxt->nworkers > 0)
	{
		/* 获取（或创建）每会话动态共享内存段的句柄。 */
		session_dsm_handle = GetSessionDsmHandle();

		/*
		 * 若无法创建每会话动态共享内存段，仍可继续，但
		 * 无法安全地启动任何工作进程，因为它们的 record typemod
		 * 将不兼容，因而无法交换
		 * 元组。
		 */
		if (session_dsm_handle == DSM_HANDLE_INVALID)
			pcxt->nworkers = 0;
	}

	if (pcxt->nworkers > 0)
	{
		/* 为各类状态共享估计空间。 */
		library_len = EstimateLibraryStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, library_len);
		guc_len = EstimateGUCStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, guc_len);
		combocidlen = EstimateComboCIDStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, combocidlen);
		if (IsolationUsesXactSnapshot())
		{
			tsnaplen = EstimateSnapshotSpace(transaction_snapshot);
			shm_toc_estimate_chunk(&pcxt->estimator, tsnaplen);
		}
		asnaplen = EstimateSnapshotSpace(active_snapshot);
		shm_toc_estimate_chunk(&pcxt->estimator, asnaplen);
		tstatelen = EstimateTransactionStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, tstatelen);
		shm_toc_estimate_chunk(&pcxt->estimator, sizeof(dsm_handle));
		pendingsyncslen = EstimatePendingSyncsSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, pendingsyncslen);
		reindexlen = EstimateReindexStateSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, reindexlen);
		relmapperlen = EstimateRelationMapSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, relmapperlen);
		uncommittedenumslen = EstimateUncommittedEnumsSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, uncommittedenumslen);
		clientconninfolen = EstimateClientConnectionInfoSpace();
		shm_toc_estimate_chunk(&pcxt->estimator, clientconninfolen);
		/* 若在此添加更多块，多半还需要添加键。 */
		shm_toc_estimate_keys(&pcxt->estimator, 12);

		/* 为错误队列估计所需空间。 */
		StaticAssertStmt(BUFFERALIGN(PARALLEL_ERROR_QUEUE_SIZE) ==
						 PARALLEL_ERROR_QUEUE_SIZE,
						 "parallel error queue size not buffer-aligned");
		shm_toc_estimate_chunk(&pcxt->estimator,
							   mul_size(PARALLEL_ERROR_QUEUE_SIZE,
										pcxt->nworkers));
		shm_toc_estimate_keys(&pcxt->estimator, 1);

		/* 估计入口点信息需要多少空间。 */
		shm_toc_estimate_chunk(&pcxt->estimator, strlen(pcxt->library_name) +
							   strlen(pcxt->function_name) + 2);
		shm_toc_estimate_keys(&pcxt->estimator, 1);
	}

	/*
	 * 创建动态共享内存并用新的目录表初始化。但若用户
	 * 未请求任何工作进程，就不必创建动态共享
	 * 内存段；改为使用后端私有内存即可。
	 *
	 * 另外，若因段数已达上限而无法创建动态共享内存段，
	 * 则回退到后端私有内存，并计划不使用任何工作进程。我们希望
	 * 这种情况不常发生，但与其直接失败，不如放弃使用
	 * 并行——放弃并行性总比直接失败
	 * 要好。
	 */
	segsize = shm_toc_estimate(&pcxt->estimator);
	if (pcxt->nworkers > 0)
		pcxt->seg = dsm_create(segsize, DSM_CREATE_NULL_IF_MAXSEGMENTS);
	if (pcxt->seg != NULL)
		pcxt->toc = shm_toc_create(PARALLEL_MAGIC,
								   dsm_segment_address(pcxt->seg),
								   segsize);
	else
	{
		pcxt->nworkers = 0;
		pcxt->private_memory = MemoryContextAlloc(TopMemoryContext, segsize);
		pcxt->toc = shm_toc_create(PARALLEL_MAGIC, pcxt->private_memory,
								   segsize);
	}

	/* 在共享内存中初始化固定大小的状态。 */
	fps = (FixedParallelState *)
		shm_toc_allocate(pcxt->toc, sizeof(FixedParallelState));
	fps->database_id = MyDatabaseId;
	fps->authenticated_user_id = GetAuthenticatedUserId();
	fps->session_user_id = GetSessionUserId();
	fps->outer_user_id = GetCurrentRoleId();
	GetUserIdAndSecContext(&fps->current_user_id, &fps->sec_context);
	fps->authenticated_user_is_superuser = GetAuthenticatedUserIsSuperuser();
	fps->session_user_is_superuser = GetSessionUserIsSuperuser();
	fps->role_is_superuser = session_auth_is_superuser;
	GetTempNamespaceState(&fps->temp_namespace_id,
						  &fps->temp_toast_namespace_id);
	fps->parallel_leader_pgproc = MyProc;
	fps->parallel_leader_pid = MyProcPid;
	fps->parallel_leader_backend_id = MyBackendId;
	fps->xact_ts = GetCurrentTransactionStartTimestamp();
	fps->stmt_ts = GetCurrentStatementStartTimestamp();
	fps->serializable_xact_handle = ShareSerializableXact();
	SpinLockInit(&fps->mutex);
	fps->last_xlog_end = 0;
	shm_toc_insert(pcxt->toc, PARALLEL_KEY_FIXED, fps);

	/* 若未为任何工作进程预留预算，可跳过其余部分。 */
	if (pcxt->nworkers > 0)
	{
		char	   *libraryspace;
		char	   *gucspace;
		char	   *combocidspace;
		char	   *tsnapspace;
		char	   *asnapspace;
		char	   *tstatespace;
		char	   *pendingsyncsspace;
		char	   *reindexspace;
		char	   *relmapperspace;
		char	   *error_queue_space;
		char	   *session_dsm_handle_space;
		char	   *entrypointstate;
		char	   *uncommittedenumsspace;
		char	   *clientconninfospace;
		Size		lnamelen;

		/* 序列化我们已加载的共享库。 */
		libraryspace = shm_toc_allocate(pcxt->toc, library_len);
		SerializeLibraryState(library_len, libraryspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_LIBRARY, libraryspace);

		/* 序列化 GUC 设置。 */
		gucspace = shm_toc_allocate(pcxt->toc, guc_len);
		SerializeGUCState(guc_len, gucspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_GUC, gucspace);

		/* 序列化 combo CID 状态。 */
		combocidspace = shm_toc_allocate(pcxt->toc, combocidlen);
		SerializeComboCIDState(combocidlen, combocidspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_COMBO_CID, combocidspace);

		/*
		 * 若事务隔离级别使用事务快照，则序列化
		 * 事务快照。
		 */
		if (IsolationUsesXactSnapshot())
		{
			tsnapspace = shm_toc_allocate(pcxt->toc, tsnaplen);
			SerializeSnapshot(transaction_snapshot, tsnapspace);
			shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT,
						   tsnapspace);
		}

		/* 序列化活动快照。 */
		asnapspace = shm_toc_allocate(pcxt->toc, asnaplen);
		SerializeSnapshot(active_snapshot, asnapspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ACTIVE_SNAPSHOT, asnapspace);

		/* 提供每会话段的句柄。 */
		session_dsm_handle_space = shm_toc_allocate(pcxt->toc,
													sizeof(dsm_handle));
		*(dsm_handle *) session_dsm_handle_space = session_dsm_handle;
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_SESSION_DSM,
					   session_dsm_handle_space);

		/* 序列化事务状态。 */
		tstatespace = shm_toc_allocate(pcxt->toc, tstatelen);
		SerializeTransactionState(tstatelen, tstatespace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_TRANSACTION_STATE, tstatespace);

		/* 序列化待处理的同步。 */
		pendingsyncsspace = shm_toc_allocate(pcxt->toc, pendingsyncslen);
		SerializePendingSyncs(pendingsyncslen, pendingsyncsspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_PENDING_SYNCS,
					   pendingsyncsspace);

		/* 序列化重建索引状态。 */
		reindexspace = shm_toc_allocate(pcxt->toc, reindexlen);
		SerializeReindexState(reindexlen, reindexspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_REINDEX_STATE, reindexspace);

		/* 序列化关系映射器状态。 */
		relmapperspace = shm_toc_allocate(pcxt->toc, relmapperlen);
		SerializeRelationMap(relmapperlen, relmapperspace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_RELMAPPER_STATE,
					   relmapperspace);

		/* 序列化未提交枚举状态。 */
		uncommittedenumsspace = shm_toc_allocate(pcxt->toc,
												 uncommittedenumslen);
		SerializeUncommittedEnums(uncommittedenumsspace, uncommittedenumslen);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_UNCOMMITTEDENUMS,
					   uncommittedenumsspace);

		/* 序列化我们的 ClientConnectionInfo。 */
		clientconninfospace = shm_toc_allocate(pcxt->toc, clientconninfolen);
		SerializeClientConnectionInfo(clientconninfolen, clientconninfospace);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_CLIENTCONNINFO,
					   clientconninfospace);

		/* 为工作进程信息分配空间。 */
		pcxt->worker = palloc0(sizeof(ParallelWorkerInfo) * pcxt->nworkers);

		/*
		 * 在动态共享内存中建立错误队列。
		 *
		 * 这些队列应仅用于传输 ErrorResponse、
		 * NoticeResponse 和 NotifyResponse 协议消息。元组数据
		 * 应通过单独的（可能更大的？）队列传输。
		 */
		error_queue_space =
			shm_toc_allocate(pcxt->toc,
							 mul_size(PARALLEL_ERROR_QUEUE_SIZE,
									  pcxt->nworkers));
		for (i = 0; i < pcxt->nworkers; ++i)
		{
			char	   *start;
			shm_mq	   *mq;

			start = error_queue_space + i * PARALLEL_ERROR_QUEUE_SIZE;
			mq = shm_mq_create(start, PARALLEL_ERROR_QUEUE_SIZE);
			shm_mq_set_receiver(mq, MyProc);
			pcxt->worker[i].error_mqh = shm_mq_attach(mq, pcxt->seg, NULL);
		}
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ERROR_QUEUE, error_queue_space);

		/*
		 * 序列化入口点信息。跨进程传递函数指针不安全，因为在
		 * EXEC_BACKEND 构建中每个进程里的函数指针可能不同，因此我们
		 * 总是传递库名和函数名。（核心后端中的函数使用库名
		 * "postgres"。）
		 * 绝不以函数指针形式跨进程传递入口点。
		 */
		lnamelen = strlen(pcxt->library_name);
		entrypointstate = shm_toc_allocate(pcxt->toc, lnamelen +
										   strlen(pcxt->function_name) + 2);
		strcpy(entrypointstate, pcxt->library_name);
		strcpy(entrypointstate + lnamelen + 1, pcxt->function_name);
		shm_toc_insert(pcxt->toc, PARALLEL_KEY_ENTRYPOINT, entrypointstate);
	}

	/* 更新 nworkers_to_launch，以防上面改过 nworkers。 */
	pcxt->nworkers_to_launch = pcxt->nworkers;

	/* 恢复先前的内存上下文。 */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * 重新初始化并行上下文的动态共享内存段，以便
 * 可以再次为其启动工作进程。
 */
void
ReinitializeParallelDSM(ParallelContext *pcxt)
{
	FixedParallelState *fps;

	/* 等待任何旧工作进程退出。 */
	if (pcxt->nworkers_launched > 0)
	{
		WaitForParallelWorkersToFinish(pcxt);
		WaitForParallelWorkersToExit(pcxt);
		pcxt->nworkers_launched = 0;
		if (pcxt->known_attached_workers)
		{
			pfree(pcxt->known_attached_workers);
			pcxt->known_attached_workers = NULL;
			pcxt->nknown_attached_workers = 0;
		}
	}

	/* 把固定并行状态的若干字段重置为干净状态。 */
	fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED, false);
	fps->last_xlog_end = 0;

	/* 重新创建错误队列（若存在）。 */
	if (pcxt->nworkers > 0)
	{
		char	   *error_queue_space;
		int			i;

		error_queue_space =
			shm_toc_lookup(pcxt->toc, PARALLEL_KEY_ERROR_QUEUE, false);
		for (i = 0; i < pcxt->nworkers; ++i)
		{
			char	   *start;
			shm_mq	   *mq;

			start = error_queue_space + i * PARALLEL_ERROR_QUEUE_SIZE;
			mq = shm_mq_create(start, PARALLEL_ERROR_QUEUE_SIZE);
			shm_mq_set_receiver(mq, MyProc);
			pcxt->worker[i].error_mqh = shm_mq_attach(mq, pcxt->seg, NULL);
		}
	}
}

/*
 * 重新初始化并行上下文的并行工作进程，以便可以
 * 启动不同数量的工作进程。需要在复用同一动态共享内存段、但
 * 工作进程数可能每次运行不同的情况下
 * 使用。
 */
void
ReinitializeParallelWorkers(ParallelContext *pcxt, int nworkers_to_launch)
{
	/*
	 * 需要启动的工作进程数必须小于初始化并行上下文时的
	 * 工作进程数。但调用方可能不知道 InitializeParallelDSM 缩减了
	 * nworkers，因此静默裁剪该
	 * 请求即可。
	 */
	pcxt->nworkers_to_launch = Min(pcxt->nworkers, nworkers_to_launch);
}

/*
 * 启动并行工作进程。
 */
void
LaunchParallelWorkers(ParallelContext *pcxt)
{
	MemoryContext oldcontext;
	BackgroundWorker worker;
	int			i;
	bool		any_registrations_failed = false;

	/* 若没有工作进程则跳过。 */
	if (pcxt->nworkers == 0 || pcxt->nworkers_to_launch == 0)
		return;

	/* 我们需要成为锁组领导者。 */
	BecomeLockGroupLeader();

	/* 若确有工作进程，最好已有动态共享内存段。 */
	Assert(pcxt->seg != NULL);

	/* 我们可能运行在短寿命的内存上下文中。 */
	oldcontext = MemoryContextSwitchTo(TopTransactionContext);

	/* 配置一个工作进程。 */
	memset(&worker, 0, sizeof(worker));
	snprintf(worker.bgw_name, BGW_MAXLEN, "parallel worker for PID %d",
			 MyProcPid);
	snprintf(worker.bgw_type, BGW_MAXLEN, "parallel worker");
	worker.bgw_flags =
		BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION
		| BGWORKER_CLASS_PARALLEL;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	sprintf(worker.bgw_library_name, "postgres");
	sprintf(worker.bgw_function_name, "ParallelWorkerMain");
	worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(pcxt->seg));
	worker.bgw_notify_pid = MyProcPid;

	/*
	 * 启动工作进程。
	 *
	 * 调用方必须能够容忍最终工作进程数少于预期，因此注册失败时
	 * 无需在此抛错。反正帮助也不大，因为注册工作进程
	 * 绝不保证它能成功启动并
	 * 初始化。
	 */
	for (i = 0; i < pcxt->nworkers_to_launch; ++i)
	{
		memcpy(worker.bgw_extra, &i, sizeof(int));
		if (!any_registrations_failed &&
			RegisterDynamicBackgroundWorker(&worker,
											&pcxt->worker[i].bgwhandle))
		{
			shm_mq_set_handle(pcxt->worker[i].error_mqh,
							  pcxt->worker[i].bgwhandle);
			pcxt->nworkers_launched++;
		}
		else
		{
			/*
			 * 若无法注册工作进程，说明已碰到
			 * max_worker_processes 上限，以后的注册多半也会失败，因此安排跳过它们。
			 * 但仍须对其余槽位执行本代码，以确保忘掉为那些
			 * 工作进程预留的错误队列。否则我们会等待它们启动，
			 * 而它们永远不会启动。
			 * 必须清理其余槽位上预算好的错误队列，
			 * 以免永远等下去。
			 */
			any_registrations_failed = true;
			pcxt->worker[i].bgwhandle = NULL;
			shm_mq_detach(pcxt->worker[i].error_mqh);
			pcxt->worker[i].error_mqh = NULL;
		}
	}

	/*
	 * 现在 nworkers_launched 已取最终值，可以初始化
	 * known_attached_workers。
	 */
	if (pcxt->nworkers_launched > 0)
	{
		pcxt->known_attached_workers =
			palloc0(sizeof(bool) * pcxt->nworkers_launched);
		pcxt->nknown_attached_workers = 0;
	}

	/* 恢复先前的内存上下文。 */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * 等待所有工作进程挂接到其错误队列；若任一工作进程未能做到，
 * 则抛错。
 *
 * 调用方可假定：若本函数成功返回，则
 * pcxt->nworkers_launched 所给出的工作进程数已初始化并
 * 挂接到其错误队列。这些工作进程是否保证仍在运行取决于
 * 调用方要求它们运行的代码；本函数不保证它们尚未退出。不过，它
 * 保证任何已退出的工作进程必定是干净退出，并且是在
 * 成功完成所分配的工作之后。
 * （是否仍在运行取决于调用方交给它们的代码。）
 *
 * 若不调用本函数，则部分已启动的工作进程可能因
 * fork() 失败而未真正启动，或可能在早期启动阶段、
 * 挂接错误队列之前就已退出，因此 nworkers_launched
 * 无法视为完全可靠。它绝不会小于实际启动的工作进程数，
 * 但可能更大。任何未能启动的工作进程仍会由
 * WaitForParallelWorkersToFinish 发现，并在到达该函数时抛错，
 * 前提是最终会执行到该函数。
 * 在此之前不宜把 nworkers_launched 当作完全可靠的值。
 *
 * 一般而言，领导进程应在调用本函数之前尽可能多做工作。
 * fork() 失败及其他早期启动失败很少见，让领导者空闲而本可做
 * 有用工作是不可取的。不过，若领导者需要等待
 * 其全部工作进程或某个特定工作进程，可能希望在等待前先调用本
 * 函数。否则，它必须为未能启动的情况另作安排，以免永远等待。另一方面，
 * 从不等待尚未启动的工作进程、或至少从不在
 * WaitForParallelWorkersToFinish() 之前等待的领导者，完全不必
 * 调用本函数。
 * 对这类领导者而言，跳过本函数是可以接受的。
 * 因为它不会在工作进程可能尚未启动时去等待它们。
 */
void
WaitForParallelWorkersToAttach(ParallelContext *pcxt)
{
	int			i;

	/* 若没有已启动的工作进程则跳过。 */
	if (pcxt->nworkers_launched == 0)
		return;

	for (;;)
	{
		/*
		 * 这将处理任何待处理的并行消息，并且也可能
		 * 抛出从工作进程传播来的错误。
		 */
		CHECK_FOR_INTERRUPTS();

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			BgwHandleStatus status;
			shm_mq	   *mq;
			int			rc;
			pid_t		pid;

			if (pcxt->known_attached_workers[i])
				continue;

			/*
			 * 若 error_mqh 为 NULL，则工作进程已
			 * 干净退出。
			 */
			if (pcxt->worker[i].error_mqh == NULL)
			{
				pcxt->known_attached_workers[i] = true;
				++pcxt->nknown_attached_workers;
				continue;
			}

			status = GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle, &pid);
			if (status == BGWH_STARTED)
			{
				/* 工作进程是否已挂接到错误队列？ */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) != NULL)
				{
					/* 是，因此已知已挂接。 */
					pcxt->known_attached_workers[i] = true;
					++pcxt->nknown_attached_workers;
				}
			}
			else if (status == BGWH_STOPPED)
			{
				/*
				 * 若工作进程在未挂接错误队列的情况下停止，
				 * 则抛错。
				 */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("parallel worker failed to initialize"),
							 errhint("More details may be available in the server log.")));

				pcxt->known_attached_workers[i] = true;
				++pcxt->nknown_attached_workers;
			}
			else
			{
				/*
				 * 工作进程尚未启动，因此必须等待。若工作进程状态变化，
				 * postmaster 会通知我们。我们的锁存器也可能因其他原因被置位，
				 * 但若如此我们只会再次等待同一个
				 * 工作进程。
				 */
				rc = WaitLatch(MyLatch,
							   WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
							   -1, WAIT_EVENT_BGWORKER_STARTUP);

				if (rc & WL_LATCH_SET)
					ResetLatch(MyLatch);
			}
		}

		/* 若已知所有工作进程都已启动，我们就完成了。 */
		if (pcxt->nknown_attached_workers >= pcxt->nworkers_launched)
		{
			Assert(pcxt->nknown_attached_workers == pcxt->nworkers_launched);
			break;
		}
	}
}

/*
 * 等待所有工作进程完成计算。
 *
 * 即使并行操作似乎已成功完成，之后仍须
 * 调用本函数。我们绝不能漏掉工作进程在并行操作期间可能抛出的
 * 任何错误，或它们在关闭时还可能抛出的错误。
 * 关闭阶段的错误同样不能忽略。
 *
 * 另外，我们要根据工作进程的反馈更新我们对
 * XactLastRecEnd 的认知。
 */
void
WaitForParallelWorkersToFinish(ParallelContext *pcxt)
{
	for (;;)
	{
		bool		anyone_alive = false;
		int			nfinished = 0;
		int			i;

		/*
		 * 这将处理任何待处理的并行消息，可能
		 * 改变后续循环的结果。它也可能抛出从工作进程
		 * 传播来的错误。
		 */
		CHECK_FOR_INTERRUPTS();

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			/*
			 * 若 error_mqh 为 NULL，则工作进程已
			 * 干净退出。若我们已通过 error_mqh 收到来自
			 * 该工作进程的消息，就知道它干净启动，因此当它退出时
			 * 我们一定会被通知。
			 */
			if (pcxt->worker[i].error_mqh == NULL)
				++nfinished;
			else if (pcxt->known_attached_workers[i])
			{
				anyone_alive = true;
				break;
			}
		}

		if (!anyone_alive)
		{
			/* 若已知所有工作进程都已结束，我们就完成了。 */
			if (nfinished >= pcxt->nworkers_launched)
			{
				Assert(nfinished == pcxt->nworkers_launched);
				break;
			}

			/*
			 * 我们未检测到任何仍存活的工作进程，但并非所有工作进程都
			 * 已知已干净退出。要么尚未全部启动，要么
			 * 有些未能启动或异常
			 * 终止。
			 */
			for (i = 0; i < pcxt->nworkers_launched; ++i)
			{
				pid_t		pid;
				shm_mq	   *mq;

				/*
				 * 若工作进程处于 BGWH_NOT_YET_STARTED 或 BGWH_STARTED，我们
				 * 应继续等待。若为 BGWH_STOPPED，则需要
				 * 进一步调查。
				 */
				if (pcxt->worker[i].error_mqh == NULL ||
					pcxt->worker[i].bgwhandle == NULL ||
					GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle,
										   &pid) != BGWH_STOPPED)
					continue;

				/*
				 * 检查工作进程是否在从未挂接错误队列的情况下
				 * 已经停住。若是，则 postmaster 无法
				 * fork 该工作进程，或它在未正确初始化的情况下退出。
				 * 我们必须抛错，因为调用方可能一直期望该工作进程在
				 * 退出前完成某些
				 * 工作。
				 */
				mq = shm_mq_get_queue(pcxt->worker[i].error_mqh);
				if (shm_mq_get_sender(mq) == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("parallel worker failed to initialize"),
							 errhint("More details may be available in the server log.")));

				/*
				 * 工作进程已停止，但仍挂接在错误队列上。
				 * 除非某处有缺陷，否则这只会在工作进程于本函数靠近顶部的
				 * CHECK_FOR_INTERRUPTS() 之后、调用 GetBackgroundWorkerPid() 之前
				 * 写入消息并终止时发生。在那种情况下，
				 * 我们的锁存器也应已被置位，下一轮循环会做正确的
				 * 事。
				 * 下一轮循环会正确处理这种情况。
				 */
			}
		}

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, -1,
						 WAIT_EVENT_PARALLEL_FINISH);
		ResetLatch(MyLatch);
	}

	if (pcxt->toc != NULL)
	{
		FixedParallelState *fps;

		fps = shm_toc_lookup(pcxt->toc, PARALLEL_KEY_FIXED, false);
		if (fps->last_xlog_end > XactLastRecEnd)
			XactLastRecEnd = fps->last_xlog_end;
	}
}

/*
 * 等待所有工作进程退出。
 *
 * 本函数确保工作进程已完全关闭。
 * WaitForParallelWorkersToFinish 与本函数的区别在于：
 * 前者只确保领导后端收到工作进程后端发送的最后一条消息，
 * 而本函数确保完全关闭。
 */
static void
WaitForParallelWorkersToExit(ParallelContext *pcxt)
{
	int			i;

	/* 等到工作进程真正死亡。 */
	for (i = 0; i < pcxt->nworkers_launched; ++i)
	{
		BgwHandleStatus status;

		if (pcxt->worker == NULL || pcxt->worker[i].bgwhandle == NULL)
			continue;

		status = WaitForBackgroundWorkerShutdown(pcxt->worker[i].bgwhandle);

		/*
		 * 若 postmaster 挂了，我们就没有机会安全清理——
		 * 无法得知工作进程何时真正死亡。这不必
		 * PANIC，因为它们最终都会中止，但我们不能安全地继续本
		 * 会话。
		 */
		if (status == BGWH_POSTMASTER_DIED)
			ereport(FATAL,
					(errcode(ERRCODE_ADMIN_SHUTDOWN),
					 errmsg("postmaster exited during a parallel transaction")));

		/* 释放内存。 */
		pfree(pcxt->worker[i].bgwhandle);
		pcxt->worker[i].bgwhandle = NULL;
	}
}

/*
 * 销毁并行上下文。
 *
 * 若期望干净退出，应先使用 WaitForParallelWorkersToFinish()，
 * 再调用本函数。调用本函数时，任何仍存活的工作进程会被强行杀死；
 * 动态共享内存段会被取消映射；然后我们（不可中断地）等待
 * 工作进程退出。
 */
void
DestroyParallelContext(ParallelContext *pcxt)
{
	int			i;

	/*
	 * 此处操作顺序要小心！我们先从列表中移除并行
	 * 上下文，再做其他事；否则若后续步骤出错，
	 * 可能会从 AtEOXact_Parallel 或 AtEOSubXact_Parallel
	 * 再次试图销毁它。
	 */
	dlist_delete(&pcxt->node);

	/* 依次杀死每个工作进程，并忘掉它们的错误队列。 */
	if (pcxt->worker != NULL)
	{
		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			if (pcxt->worker[i].error_mqh != NULL)
			{
				TerminateBackgroundWorker(pcxt->worker[i].bgwhandle);

				shm_mq_detach(pcxt->worker[i].error_mqh);
				pcxt->worker[i].error_mqh = NULL;
			}
		}
	}

	/*
	 * 若我们已分配共享内存段，则脱离它。这会
	 * 隐式脱离存放在其中的错误队列以及任何其他
	 * 共享内存队列。
	 */
	if (pcxt->seg != NULL)
	{
		dsm_detach(pcxt->seg);
		pcxt->seg = NULL;
	}

	/*
	 * 若此并行上下文实际位于后端私有内存而非
	 * 共享内存中，则改为释放那块内存。
	 */
	if (pcxt->private_memory != NULL)
	{
		pfree(pcxt->private_memory);
		pcxt->private_memory = NULL;
	}

	/*
	 * 在所有工作进程退出之前，我们无法完成事务提交或中止。
	 * 这尤其意味着，在此阶段我们无法响应
	 * 中断。
	 */
	HOLD_INTERRUPTS();
	WaitForParallelWorkersToExit(pcxt);
	RESUME_INTERRUPTS();

	/* 释放工作进程数组本身。 */
	if (pcxt->worker != NULL)
	{
		pfree(pcxt->worker);
		pcxt->worker = NULL;
	}

	/* 释放内存。 */
	pfree(pcxt->library_name);
	pfree(pcxt->function_name);
	pfree(pcxt);
}

/*
 * 当前是否有任何活动的并行上下文？
 */
bool
ParallelContextActive(void)
{
	return !dlist_is_empty(&pcxt_list);
}

/*
 * 处理指示有并行工作进程消息的中断。
 *
 * 注意：这是在信号处理器内调用的！我们所能做的只是置位
 * 一个标志，使下一次 CHECK_FOR_INTERRUPTS() 调用
 * HandleParallelMessages()。
 */
void
HandleParallelMessageInterrupt(void)
{
	InterruptPending = true;
	ParallelMessagePending = true;
	SetLatch(MyLatch);
}

/*
 * 处理从并行工作进程收到的任何排队协议消息。
 */
void
HandleParallelMessages(void)
{
	dlist_iter	iter;
	MemoryContext oldcontext;

	static MemoryContext hpm_context = NULL;

	/*
	 * 这从 ProcessInterrupts() 调用；由于它所调用的部分
	 * 函数包含 CHECK_FOR_INTERRUPTS()，若在运行期间收到更多信号，
	 * 存在递归调用的可能。不清楚递归进入是否安全，
	 * 即便安全也似乎无用，因此完成前阻塞
	 * 中断。
	 */
	HOLD_INTERRUPTS();

	/*
	 * 此外，CurrentMemoryContext 可能指向几乎任何地方。我们
	 * 不想把数据泄漏到长寿命上下文中，因此在此于私有
	 * 上下文中完成工作，每次使用时可重置。
	 */
	if (hpm_context == NULL)	/* 第一次经过？ */
		hpm_context = AllocSetContextCreate(TopMemoryContext,
											"HandleParallelMessages",
											ALLOCSET_DEFAULT_SIZES);
	else
		MemoryContextReset(hpm_context);

	oldcontext = MemoryContextSwitchTo(hpm_context);

	/* 可以处理消息了。重置表示还有更多待处理的标志。 */
	ParallelMessagePending = false;

	dlist_foreach(iter, &pcxt_list)
	{
		ParallelContext *pcxt;
		int			i;

		pcxt = dlist_container(ParallelContext, node, iter.cur);
		if (pcxt->worker == NULL)
			continue;

		for (i = 0; i < pcxt->nworkers_launched; ++i)
		{
			/*
			 * 尽量从每个工作进程读取尽可能多的消息，但在以下情况停止：
			 * (1) 工作进程的错误队列消失，这可能在我们收到工作进程的
			 * Terminate 消息时发生；或 (2) 若不阻塞就无法再从该
			 * 工作进程读取更多消息。
			 */
			while (pcxt->worker[i].error_mqh != NULL)
			{
				shm_mq_result res;
				Size		nbytes;
				void	   *data;

				res = shm_mq_receive(pcxt->worker[i].error_mqh, &nbytes,
									 &data, true);
				if (res == SHM_MQ_WOULD_BLOCK)
					break;
				else if (res == SHM_MQ_SUCCESS)
				{
					StringInfoData msg;

					initStringInfo(&msg);
					appendBinaryStringInfo(&msg, data, nbytes);
					HandleParallelMessage(pcxt, i, &msg);
					pfree(msg.data);
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							 errmsg("lost connection to parallel worker")));
			}
		}
	}

	MemoryContextSwitchTo(oldcontext);

	/* 离开时不妨清理该上下文 */
	MemoryContextReset(hpm_context);

	RESUME_INTERRUPTS();
}

/*
 * 处理从单个并行工作进程收到的单条协议消息。
 */
static void
HandleParallelMessage(ParallelContext *pcxt, int i, StringInfo msg)
{
	char		msgtype;

	if (pcxt->known_attached_workers != NULL &&
		!pcxt->known_attached_workers[i])
	{
		pcxt->known_attached_workers[i] = true;
		pcxt->nknown_attached_workers++;
	}

	msgtype = pq_getmsgbyte(msg);

	switch (msgtype)
	{
		case 'K':				/* BackendKeyData */
			{
				int32		pid = pq_getmsgint(msg, 4);

				(void) pq_getmsgint(msg, 4);	/* 丢弃取消键 */
				(void) pq_getmsgend(msg);
				pcxt->worker[i].pid = pid;
				break;
			}

		case 'E':				/* ErrorResponse */
		case 'N':				/* NoticeResponse */
			{
				ErrorData	edata;
				ErrorContextCallback *save_error_context_stack;

				/* 解析 ErrorResponse 或 NoticeResponse。 */
				pq_parse_errornotice(msg, &edata);

				/* 工作进程死亡不足以成为自杀的正当理由。 */
				edata.elevel = Min(edata.elevel, ERROR);

				/*
				 * 若需要，添加一行上下文以表明这是从并行工作进程
				 * 传播来的消息。否则有时很难理解实际
				 * 发生了什么。（在 DEBUG_PARALLEL_REGRESS 模式下我们不做此事，
				 * 因为它会因是否实际使用并行工作进程而导致测试结果
				 * 不稳定。）
				 * 该模式下跳过是为了保持回归测试稳定。
				 */
				if (debug_parallel_query != DEBUG_PARALLEL_REGRESS)
				{
					if (edata.context)
						edata.context = psprintf("%s\n%s", edata.context,
												 _("parallel worker"));
					else
						edata.context = pstrdup(_("parallel worker"));
				}

				/*
				 * 除此之外的上下文应使用创建 ParallelContext 时生效的
				 * 错误上下文回调，而不是当前的
				 * 那些回调。
				 */
				save_error_context_stack = error_context_stack;
				error_context_stack = pcxt->error_context_stack;

				/* 重新抛出错误或打印通知。 */
				ThrowErrorData(&edata);

				/* 不是错误，因此恢复先前的上下文栈。 */
				error_context_stack = save_error_context_stack;

				break;
			}

		case 'A':				/* NotifyResponse */
			{
				/* 传播 NotifyResponse。 */
				int32		pid;
				const char *channel;
				const char *payload;

				pid = pq_getmsgint(msg, 4);
				channel = pq_getmsgrawstring(msg);
				payload = pq_getmsgrawstring(msg);
				pq_endmessage(msg);

				NotifyMyFrontEnd(channel, payload, pid);

				break;
			}

		case 'X':				/* Terminate，表示干净退出 */
			{
				shm_mq_detach(pcxt->worker[i].error_mqh);
				pcxt->worker[i].error_mqh = NULL;
				break;
			}

		default:
			{
				elog(ERROR, "unrecognized message type received from parallel worker: %c (message length %d bytes)",
					 msgtype, msg->len);
			}
	}
}

/*
 * 并行上下文的子事务结束清理。
 *
 * 目前禁止在并行模式生效时进入或离开子事务，
 * 因此我们本可直接清掉一切。但将来可能想放宽该限制，
 * 因此本代码考虑到 pcxt_list 中可能有多个
 * 子事务 ID。
 */
void
AtEOSubXact_Parallel(bool isCommit, SubTransactionId mySubId)
{
	while (!dlist_is_empty(&pcxt_list))
	{
		ParallelContext *pcxt;

		pcxt = dlist_head_element(ParallelContext, node, &pcxt_list);
		if (pcxt->subid != mySubId)
			break;
		if (isCommit)
			elog(WARNING, "leaked parallel context");
		DestroyParallelContext(pcxt);
	}
}

/*
 * 并行上下文的事务结束清理。
 */
void
AtEOXact_Parallel(bool isCommit)
{
	while (!dlist_is_empty(&pcxt_list))
	{
		ParallelContext *pcxt;

		pcxt = dlist_head_element(ParallelContext, node, &pcxt_list);
		if (isCommit)
			elog(WARNING, "leaked parallel context");
		DestroyParallelContext(pcxt);
	}
}

/*
 * 并行工作进程的主入口点。
 */
void
ParallelWorkerMain(Datum main_arg)
{
	dsm_segment *seg;
	shm_toc    *toc;
	FixedParallelState *fps;
	char	   *error_queue_space;
	shm_mq	   *mq;
	shm_mq_handle *mqh;
	char	   *libraryspace;
	char	   *entrypointstate;
	char	   *library_name;
	char	   *function_name;
	parallel_worker_main_type entrypt;
	char	   *gucspace;
	char	   *combocidspace;
	char	   *tsnapspace;
	char	   *asnapspace;
	char	   *tstatespace;
	char	   *pendingsyncsspace;
	char	   *reindexspace;
	char	   *relmapperspace;
	char	   *uncommittedenumsspace;
	char	   *clientconninfospace;
	StringInfoData msgbuf;
	char	   *session_dsm_handle_space;
	Snapshot	tsnapshot;
	Snapshot	asnapshot;

	/* 置位标志以表明我们正在初始化并行工作进程。 */
	InitializingParallelWorker = true;

	/* 建立信号处理器。 */
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();

	/* 确定并设置我们的并行工作进程编号。 */
	Assert(ParallelWorkerNumber == -1);
	memcpy(&ParallelWorkerNumber, MyBgworkerEntry->bgw_extra, sizeof(int));

	/* 为整洁起见，建立一个用于工作的内存上下文。 */
	CurrentMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "Parallel worker",
												 ALLOCSET_DEFAULT_SIZES);

	/*
	 * 挂接到并行查询的动态共享内存段，并
	 * 找到其目录表。
	 *
	 * 注意：此时我们尚未在本进程中创建任何 ResourceOwner。
	 * 这将导致我们的动态共享内存映射一直存活到进程退出，这没问题。
	 * 若有 ResourceOwner，它会取得该映射的所有权，但我们不需要
	 * 那样。
	 */
	seg = dsm_attach(DatumGetUInt32(main_arg));
	if (seg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not map dynamic shared memory segment")));
	toc = shm_toc_attach(PARALLEL_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("invalid magic number in dynamic shared memory segment")));

	/* 查找固定并行状态。 */
	fps = shm_toc_lookup(toc, PARALLEL_KEY_FIXED, false);
	MyFixedParallelState = fps;

	/* 安排在我们退出时向领导者发信号。 */
	ParallelLeaderPid = fps->parallel_leader_pid;
	ParallelLeaderBackendId = fps->parallel_leader_backend_id;
	before_shmem_exit(ParallelWorkerShutdown, PointerGetDatum(seg));

	/*
	 * 现在可以找到并挂接为我们提供的错误队列了。这很好，
	 * 因为在此之前，此处发生的任何错误都不会报告回
	 * 请求启动本工作进程的那个
	 * 进程。
	 */
	error_queue_space = shm_toc_lookup(toc, PARALLEL_KEY_ERROR_QUEUE, false);
	mq = (shm_mq *) (error_queue_space +
					 ParallelWorkerNumber * PARALLEL_ERROR_QUEUE_SIZE);
	shm_mq_set_sender(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);
	pq_redirect_to_shm_mq(seg, mqh);
	pq_set_parallel_leader(fps->parallel_leader_pid,
						   fps->parallel_leader_backend_id);

	/*
	 * 向发起并行的进程发送 BackendKeyData 消息，
	 * 以便它在收到我们的其他任何消息之前就能得知我们的 PID。
	 * 我们的取消键也会发送，因为协议消息就是这样定义的，
	 * 但在此情形下实际上不会用于任何
	 * 事。
	 */
	pq_beginmessage(&msgbuf, 'K');
	pq_sendint32(&msgbuf, (int32) MyProcPid);
	pq_sendint32(&msgbuf, (int32) MyCancelKey);
	pq_endmessage(&msgbuf);

	/*
	 * 太好了！初步初始化已完成。现在，我们需要设置我们的
	 * 后端本地状态以匹配原始后端。
	 */

	/*
	 * 加入锁组。我们必须在任何可能尝试获取重量级锁的操作之前
	 * 完成此事，因为到此为止获取的任何重量级锁都可能直接阻塞
	 * 并行组领导者，或阻塞某个反过来等待与并行组领导者冲突的锁的
	 * 进程，从而造成未被检测到的死锁。（若无法加入锁组，说明领导者已退出，
	 * 因此安静退出即可。）
	 * 在加入锁组之前获取重量级锁是危险的。
	 * 若无法加入则说明领导者已不在，安静退出即可。
	 */
	if (!BecomeLockGroupMember(fps->parallel_leader_pgproc,
							   fps->parallel_leader_pid))
		return;

	/*
	 * 恢复事务与语句开始时间戳。这必须在任何会
	 * 启动事务的操作之前发生，否则 xact.c 中的断言会
	 * 触发。
	 */
	SetParallelStartTimestamps(fps->xact_ts, fps->stmt_ts);

	/*
	 * 确定要调用的入口点。理论上这可能导致加载额外的库，
	 * 不过入口点多半在核心后端中，或在我们刚加载的
	 * 库中。
	 */
	entrypointstate = shm_toc_lookup(toc, PARALLEL_KEY_ENTRYPOINT, false);
	library_name = entrypointstate;
	function_name = entrypointstate + strlen(library_name) + 1;

	entrypt = LookupParallelWorkerFunction(library_name, function_name);

	/*
	 * 恢复当前会话授权与角色 ID。此处不做验证，
	 * 我们只是盲目采用领导者的状态。注意这必须在
	 * InitPostgres 之前发生，因为 InitializeSessionUserId 不会设置
	 * 这些变量。
	 */
	SetAuthenticatedUserId(fps->authenticated_user_id,
						   fps->authenticated_user_is_superuser);
	SetSessionAuthorization(fps->session_user_id,
							fps->session_user_is_superuser);
	SetCurrentRoleId(fps->outer_user_id, fps->role_is_superuser);

	/*
	 * 恢复数据库连接。我们跳过连接授权检查，理由是
	 * (a) 领导者在启动时已检查过这些事项，且
	 * (b) 我们不希望并行模式导致这些失败，因为那会使
	 * 使用并行查询计划对应用程序不透明。
	 */
	BackgroundWorkerInitializeConnectionByOid(fps->database_id,
											  fps->authenticated_user_id,
											  BGWORKER_BYPASS_ALLOWCONN);

	/*
	 * 将客户端编码设为数据库编码，因为领导者
	 * 会期望如此。
	 */
	SetClientEncoding(GetDatabaseEncoding());

	/*
	 * 加载原始后端已加载的库。我们希望在恢复 GUCs 之前
	 * 做此事，因为这些库可能定义自定义
	 * 变量。
	 */
	libraryspace = shm_toc_lookup(toc, PARALLEL_KEY_LIBRARY, false);
	StartTransactionCommand();
	RestoreLibraryState(libraryspace);

	/* 从启动后端恢复 GUC 值。 */
	gucspace = shm_toc_lookup(toc, PARALLEL_KEY_GUC, false);
	RestoreGUCState(gucspace);
	CommitTransactionCommand();

	/* 建立起适合并行工作进程的事务状态。 */
	tstatespace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_STATE, false);
	StartParallelWorkerTransaction(tstatespace);

	/* 恢复 combo CID 状态。 */
	combocidspace = shm_toc_lookup(toc, PARALLEL_KEY_COMBO_CID, false);
	RestoreComboCIDState(combocidspace);

	/* 挂接到每会话动态共享内存段及其中的对象。 */
	session_dsm_handle_space =
		shm_toc_lookup(toc, PARALLEL_KEY_SESSION_DSM, false);
	AttachSession(*(dsm_handle *) session_dsm_handle_space);

	/*
	 * 若事务隔离级别为 REPEATABLE READ 或 SERIALIZABLE，
	 * 领导者已序列化事务快照，我们必须恢复它。在较低
	 * 隔离级别下没有事务寿命期的快照，但我们需要把
	 * TransactionXmin 设为小于或等于本工作进程将使用的每个快照的
	 * xmin 的值。最简单的做法是把活动快照安装为
	 * 事务快照。在本并行工作进程中运行的代码可能通过
	 * GetTransactionSnapshot() 或 GetLatestSnapshot() 获取新快照，但不应有办法
	 * 获得比活动快照更旧的快照。
	 * 换言之，活动快照给出了可见性的下限。
	 * 工作进程不得看到比该下限更旧的快照。
	 */
	asnapspace = shm_toc_lookup(toc, PARALLEL_KEY_ACTIVE_SNAPSHOT, false);
	tsnapspace = shm_toc_lookup(toc, PARALLEL_KEY_TRANSACTION_SNAPSHOT, true);
	asnapshot = RestoreSnapshot(asnapspace);
	tsnapshot = tsnapspace ? RestoreSnapshot(tsnapspace) : asnapshot;
	RestoreTransactionSnapshot(tsnapshot,
							   fps->parallel_leader_pgproc);
	PushActiveSnapshot(asnapshot);

	/*
	 * 我们已改变能看见哪些元组，因此必须使
	 * 系统缓存失效。
	 */
	InvalidateSystemCaches();

	/*
	 * 恢复当前用户 ID 与安全上下文。此处不做验证，
	 * 我们只是盲目采用领导者的状态。这必须在恢复 GUCs 之后才能做，
	 * 否则会因恢复 session_authorization 和 role 而抱怨。
	 * （实际上，我们假定所有恢复的值都可以设置，即使我们现在处于
	 * 受限上下文中。）
	 * 即便当前处于受限上下文也假定这些值可以设置。
	 */
	SetUserIdAndSecContext(fps->current_user_id, fps->sec_context);

	/* 恢复临时命名空间状态，以确保搜索路径与领导者一致。 */
	SetTempNamespaceState(fps->temp_namespace_id,
						  fps->temp_toast_namespace_id);

	/* 恢复待处理的同步。 */
	pendingsyncsspace = shm_toc_lookup(toc, PARALLEL_KEY_PENDING_SYNCS,
									   false);
	RestorePendingSyncs(pendingsyncsspace);

	/* 恢复重建索引状态。 */
	reindexspace = shm_toc_lookup(toc, PARALLEL_KEY_REINDEX_STATE, false);
	RestoreReindexState(reindexspace);

	/* 恢复关系映射器状态。 */
	relmapperspace = shm_toc_lookup(toc, PARALLEL_KEY_RELMAPPER_STATE, false);
	RestoreRelationMap(relmapperspace);

	/* 恢复未提交的枚举。 */
	uncommittedenumsspace = shm_toc_lookup(toc, PARALLEL_KEY_UNCOMMITTEDENUMS,
										   false);
	RestoreUncommittedEnums(uncommittedenumsspace);

	/* 恢复 ClientConnectionInfo。 */
	clientconninfospace = shm_toc_lookup(toc, PARALLEL_KEY_CLIENTCONNINFO,
										 false);
	RestoreClientConnectionInfo(clientconninfospace);

	/*
	 * 在 MyClientConnectionInfo 已恢复后初始化 SystemUser。同时
	 * 确保 auth_method 实际有效，亦即 authn_id 非 NULL。
	 */
	if (MyClientConnectionInfo.authn_id)
		InitializeSystemUser(MyClientConnectionInfo.authn_id,
							 hba_authname(MyClientConnectionInfo.auth_method));

	/* 若为 SERIALIZABLE，挂接到领导者的可串行化事务。 */
	AttachSerializableXact(fps->serializable_xact_handle);

	/*
	 * 我们现已初始化全部状态；此后不应再有
	 * 变化。
	 */
	InitializingParallelWorker = false;
	EnterParallelMode();

	/*
	 * 该做真正的工作了：调用调用方提供的代码。
	 */
	entrypt(seg, toc);

	/* 必须退出并行模式才能弹出活动快照。 */
	ExitParallelMode();

	/* 必须弹出活动快照，以免 snapmgr.c 抱怨。 */
	PopActiveSnapshot();

	/* 关闭并行工作进程事务。 */
	EndParallelWorkerTransaction();

	/* 脱离每会话动态共享内存段。 */
	DetachSession();

	/* 报告成功。 */
	pq_putmessage('X', NULL, 0);
}

/*
 * 用我们写入的最后一条 WAL 记录的结束位置更新共享内存，
 * 若它大于其中已存储的值。
 */
void
ParallelWorkerReportLastRecEnd(XLogRecPtr last_xlog_end)
{
	FixedParallelState *fps = MyFixedParallelState;

	Assert(fps != NULL);
	SpinLockAcquire(&fps->mutex);
	if (fps->last_xlog_end < last_xlog_end)
		fps->last_xlog_end = last_xlog_end;
	SpinLockRelease(&fps->mutex);
}

/*
 * 确保领导者再尝试从我们的错误队列读取一次。
 * 这防范我们未干净退出且未向领导者发送
 * ErrorResponse 的情况，例如某些代码直接调用
 * proc_exit。
 *
 * 也显式脱离动态共享内存段，以便使用
 * on_dsm_detach() 的子系统有机会在统计子系统作为
 * before_shmem_exit() 钩子的一部分关闭之前发送统计信息。
 *
 * 也许有人会想，只要仔细安排挂接动态共享内存段的顺序，使
 * pgstats 段比并行查询段更晚脱离就能解决。事实证明那样行不通，因为
 * 统计哈希可能需要增长，从而分配新段，
 * 那些新段反而会更早
 * 脱离。
 */
static void
ParallelWorkerShutdown(int code, Datum arg)
{
	SendProcSignal(ParallelLeaderPid,
				   PROCSIG_PARALLEL_MESSAGE,
				   ParallelLeaderBackendId);

	dsm_detach((dsm_segment *) DatumGetPointer(arg));
}

/*
 * 查找（并可能加载）并行工作进程入口点函数。
 *
 * 对于核心代码中的函数，我们使用库名 "postgres"
 * 并查阅 InternalParallelWorkers 数组。外部函数用
 * load_external_function() 查找，必要时加载。
 *
 * 这样做的目的是跨进程边界以字符串传递函数名。
 * 我们不能传递实际函数地址，因为该函数可能在
 * 不同进程中被加载到不同地址。对可加载库中的函数这显然是风险，
 * 但在使用 EXEC_BACKEND 的平台（例如 Windows）上，即便核心代码中的函数
 * 也可能发生。
 * 地址在不同进程间可能不一致。
 *
 * 将来或许值得去掉 InternalParallelWorkers[]，
 * 改为对核心函数也应用 load_external_function()；
 * 但这会带来目前不值得处理的可移植性问题。
 */
static parallel_worker_main_type
LookupParallelWorkerFunction(const char *libraryname, const char *funcname)
{
	/*
	 * 若函数要从 postgres 本身加载，则搜索
	 * InternalParallelWorkers 数组。
	 */
	if (strcmp(libraryname, "postgres") == 0)
	{
		int			i;

		for (i = 0; i < lengthof(InternalParallelWorkers); i++)
		{
			if (strcmp(InternalParallelWorkers[i].fn_name, funcname) == 0)
				return InternalParallelWorkers[i].fn_addr;
		}

		/* 只有编程错误才会到达这里。 */
		elog(ERROR, "internal function \"%s\" not found", funcname);
	}

	/* 否则从外部库加载。 */
	return (parallel_worker_main_type)
		load_external_function(libraryname, funcname, true, NULL);
}
