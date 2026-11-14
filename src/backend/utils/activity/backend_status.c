/* ----------
 * backend_status.c
 *	  后端状态报告基础设施。
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/backend_status.c
 * ----------
 */
#include "postgres.h"

#include "access/xact.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "port/atomics.h"		/* 用于内存屏障 */
#include "storage/ipc.h"
#include "storage/proc.h"		/* 供 MyProc */
#include "storage/sinvaladt.h"
#include "utils/ascii.h"
#include "utils/backend_status.h"
#include "utils/guc.h"			/* 供 application_name */
#include "utils/memutils.h"


/* ----------
 * 后端总数（含辅助进程）
 *
 * 我们为每个可能的 BackendId 保留一个槽位，另为
 * 每种辅助进程类型各保留一个槽位。（该方案假定
 * 同一时刻任一辅助进程类型仅一个。）MaxBackends
 * 同时包括 autovacuum worker 和后台工作进程。
 * ----------
 */
#define NumBackendStatSlots (MaxBackends + NUM_AUXPROCTYPES)


/* ----------
 * GUC 参数
 * ----------
 */
bool		pgstat_track_activities = false;
int			pgstat_track_activity_query_size = 1024;


/* 公开出来，供 backend_progress.c 访问 */
PgBackendStatus *MyBEEntry = NULL;


static PgBackendStatus *BackendStatusArray = NULL;
static char *BackendAppnameBuffer = NULL;
static char *BackendClientHostnameBuffer = NULL;
static char *BackendActivityBuffer = NULL;
static Size BackendActivityBufferSize = 0;
#ifdef USE_SSL
static PgBackendSSLStatus *BackendSslStatusBuffer = NULL;
#endif
#ifdef ENABLE_GSS
static PgBackendGSSStatus *BackendGssStatusBuffer = NULL;
#endif


/* 各后端（含辅助进程）的状态 */
static LocalPgBackendStatus *localBackendStatusTable = NULL;

/* 后端总数（含辅助进程） */
static int	localNumBackends = 0;

static MemoryContext backendStatusSnapContext;


static void pgstat_beshutdown_hook(int code, Datum arg);
static void pgstat_read_current_status(void);
static void pgstat_setup_backend_status_context(void);


/*
 * 报告 CreateSharedBackendStatus 所需共享内存空间。
 */
Size
BackendStatusShmemSize(void)
{
	Size		size;

	/* BackendStatusArray: */
	size = mul_size(sizeof(PgBackendStatus), NumBackendStatSlots);
	/* BackendAppnameBuffer: */
	size = add_size(size,
					mul_size(NAMEDATALEN, NumBackendStatSlots));
	/* BackendClientHostnameBuffer: */
	size = add_size(size,
					mul_size(NAMEDATALEN, NumBackendStatSlots));
	/* BackendActivityBuffer: */
	size = add_size(size,
					mul_size(pgstat_track_activity_query_size, NumBackendStatSlots));
#ifdef USE_SSL
	/* BackendSslStatusBuffer: */
	size = add_size(size,
					mul_size(sizeof(PgBackendSSLStatus), NumBackendStatSlots));
#endif
#ifdef ENABLE_GSS
	/* BackendGssStatusBuffer: */
	size = add_size(size,
					mul_size(sizeof(PgBackendGSSStatus), NumBackendStatSlots));
#endif
	return size;
}

/*
 * 在 postmaster 启动期间初始化共享状态数组与各
 * 字符串缓冲区。
 */
void
CreateSharedBackendStatus(void)
{
	Size		size;
	bool		found;
	int			i;
	char	   *buffer;

	/* 创建或挂接到共享数组 */
	size = mul_size(sizeof(PgBackendStatus), NumBackendStatSlots);
	BackendStatusArray = (PgBackendStatus *)
		ShmemInitStruct("Backend Status Array", size, &found);

	if (!found)
	{
		/*
		 * 最先到达并初始化。
		 */
		MemSet(BackendStatusArray, 0, size);
	}

	/* 创建或挂接到共享 appname 缓冲区 */
	size = mul_size(NAMEDATALEN, NumBackendStatSlots);
	BackendAppnameBuffer = (char *)
		ShmemInitStruct("Backend Application Name Buffer", size, &found);

	if (!found)
	{
		MemSet(BackendAppnameBuffer, 0, size);

		/* 初始化 st_appname 指针。 */
		buffer = BackendAppnameBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_appname = buffer;
			buffer += NAMEDATALEN;
		}
	}

	/* 创建或挂接到共享客户端主机名缓冲区 */
	size = mul_size(NAMEDATALEN, NumBackendStatSlots);
	BackendClientHostnameBuffer = (char *)
		ShmemInitStruct("Backend Client Host Name Buffer", size, &found);

	if (!found)
	{
		MemSet(BackendClientHostnameBuffer, 0, size);

		/* 初始化 st_clienthostname 指针。 */
		buffer = BackendClientHostnameBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_clienthostname = buffer;
			buffer += NAMEDATALEN;
		}
	}

	/* 创建或挂接到共享活动缓冲区 */
	BackendActivityBufferSize = mul_size(pgstat_track_activity_query_size,
										 NumBackendStatSlots);
	BackendActivityBuffer = (char *)
		ShmemInitStruct("Backend Activity Buffer",
						BackendActivityBufferSize,
						&found);

	if (!found)
	{
		MemSet(BackendActivityBuffer, 0, BackendActivityBufferSize);

		/* 初始化 st_activity 指针。 */
		buffer = BackendActivityBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_activity_raw = buffer;
			buffer += pgstat_track_activity_query_size;
		}
	}

#ifdef USE_SSL
	/* 创建或挂接到共享 SSL 状态缓冲区 */
	size = mul_size(sizeof(PgBackendSSLStatus), NumBackendStatSlots);
	BackendSslStatusBuffer = (PgBackendSSLStatus *)
		ShmemInitStruct("Backend SSL Status Buffer", size, &found);

	if (!found)
	{
		PgBackendSSLStatus *ptr;

		MemSet(BackendSslStatusBuffer, 0, size);

		/* 初始化 st_sslstatus 指针。 */
		ptr = BackendSslStatusBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_sslstatus = ptr;
			ptr++;
		}
	}
#endif

#ifdef ENABLE_GSS
	/* 创建或挂接到共享 GSSAPI 状态缓冲区 */
	size = mul_size(sizeof(PgBackendGSSStatus), NumBackendStatSlots);
	BackendGssStatusBuffer = (PgBackendGSSStatus *)
		ShmemInitStruct("Backend GSS Status Buffer", size, &found);

	if (!found)
	{
		PgBackendGSSStatus *ptr;

		MemSet(BackendGssStatusBuffer, 0, size);

		/* 初始化 st_gssstatus 指针。 */
		ptr = BackendGssStatusBuffer;
		for (i = 0; i < NumBackendStatSlots; i++)
		{
			BackendStatusArray[i].st_gssstatus = ptr;
			ptr++;
		}
	}
#endif
}

/*
 * 初始化 pgstats 后端活动状态，并建立我们的进程
 * 退出钩子。由 InitPostgres 与 AuxiliaryProcessMain 调用。
 * 辅助进程的 MyBackendId 无效；否则须设置 MyBackendId，此时
 * 未开始任何事务（因为退出钩子必须在最后一个事务
 * 退出之后运行）。
 *
 * 注：MyDatabaseId 尚未设置；因此关闭钩子必须小心行事。
 */
void
pgstat_beinit(void)
{
	/* 初始化 MyBEEntry */
	if (MyBackendId != InvalidBackendId)
	{
		Assert(MyBackendId >= 1 && MyBackendId <= MaxBackends);
		MyBEEntry = &BackendStatusArray[MyBackendId - 1];
	}
	else
	{
		/* 必须是辅助进程 */
		Assert(MyAuxProcType != NotAnAuxProcess);

		/*
		 * 为辅助进程指派 MyBEEntry。由于它没有
		 * BackendId，槽位按辅助进程类型（MyAuxProcType）
		 * 静态分配。后端使用的槽位按 0 到
		 * MaxBackends（不含）索引，因此我们将
		 * MaxBackends + AuxProcType 作为辅助进程的槽位索引。
		 *
		 */
		MyBEEntry = &BackendStatusArray[MaxBackends + MyAuxProcType];
	}

	/* 设置进程退出钩子以做清理 */
	on_shmem_exit(pgstat_beshutdown_hook, 0);
}


/* ----------
 * pgstat_bestart() -
 *
 *	初始化本后端在 PgBackendStatus 数组中的条目。
 *	来自 InitPostgres。
 *
 *	除辅助进程外，必须为后端设置 MyBackendId、
 *	MyDatabaseId、会话 userid 与 application_name
 *	（因此本函数无法与 pgstat_beinit 合并）。
 *	须注意：若这不是辅助进程，则必须处于事务中，
 *	因为可能需要对某些字符串做编码转换。
 * ----------
 */
void
pgstat_bestart(void)
{
	volatile PgBackendStatus *vbeentry = MyBEEntry;
	PgBackendStatus lbeentry;
#ifdef USE_SSL
	PgBackendSSLStatus lsslstatus;
#endif
#ifdef ENABLE_GSS
	PgBackendGSSStatus lgssstatus;
#endif

	/* pgstats 状态必须从 pgstat_beinit() 初始化 */
	Assert(vbeentry != NULL);

	/*
	 * 为尽量缩短修改 PgBackendStatus 条目所花的时间，并
	 * 避免临界区内出错的风险，我们先把共享内存结构
	 * 复制到本地变量，然后修改本地变量中的数据，最后
	 * 再把本地变量复制回共享内存。只有最后一步必须
	 * 处于临界区内。
	 *
	 * 从共享内存复制的数据大部分反正会被覆盖，
	 * 但该结构也没有大到值得为只复制所需字段而
	 * 增加维护上的麻烦。
	 */
	memcpy(&lbeentry,
		   unvolatize(PgBackendStatus *, vbeentry),
		   sizeof(PgBackendStatus));

	/* 不过这些结构体每次都可以直接从零开始 */
#ifdef USE_SSL
	memset(&lsslstatus, 0, sizeof(lsslstatus));
#endif
#ifdef ENABLE_GSS
	memset(&lgssstatus, 0, sizeof(lgssstatus));
#endif

	/*
	 * 现在填入 lbeentry 的所有字段，但不包括属于行外
	 * 数据的字符串。那些须另行处理，见下文。
	 */
	lbeentry.st_procpid = MyProcPid;
	lbeentry.st_backendType = MyBackendType;
	lbeentry.st_proc_start_timestamp = MyStartTimestamp;
	lbeentry.st_activity_start_timestamp = 0;
	lbeentry.st_state_start_timestamp = 0;
	lbeentry.st_xact_start_timestamp = 0;
	lbeentry.st_databaseid = MyDatabaseId;

	/* 为客户端后端、wal-sender 与 bgworker 进程提供 userid */
	if (lbeentry.st_backendType == B_BACKEND
		|| lbeentry.st_backendType == B_WAL_SENDER
		|| lbeentry.st_backendType == B_BG_WORKER)
		lbeentry.st_userid = GetSessionUserId();
	else
		lbeentry.st_userid = InvalidOid;

	/*
	 * 可能没有 MyProcPort（例如当前是 autovacuum 进程）。
	 * 若是如此，则使用全零的客户端地址，特殊处理位于
	 * pg_stat_get_backend_client_addr 与 pg_stat_get_backend_client_port
	 */
	if (MyProcPort)
		memcpy(&lbeentry.st_clientaddr, &MyProcPort->raddr,
			   sizeof(lbeentry.st_clientaddr));
	else
		MemSet(&lbeentry.st_clientaddr, 0, sizeof(lbeentry.st_clientaddr));

#ifdef USE_SSL
	if (MyProcPort && MyProcPort->ssl_in_use)
	{
		lbeentry.st_ssl = true;
		lsslstatus.ssl_bits = be_tls_get_cipher_bits(MyProcPort);
		strlcpy(lsslstatus.ssl_version, be_tls_get_version(MyProcPort), NAMEDATALEN);
		strlcpy(lsslstatus.ssl_cipher, be_tls_get_cipher(MyProcPort), NAMEDATALEN);
		be_tls_get_peer_subject_name(MyProcPort, lsslstatus.ssl_client_dn, NAMEDATALEN);
		be_tls_get_peer_serial(MyProcPort, lsslstatus.ssl_client_serial, NAMEDATALEN);
		be_tls_get_peer_issuer_name(MyProcPort, lsslstatus.ssl_issuer_dn, NAMEDATALEN);
	}
	else
	{
		lbeentry.st_ssl = false;
	}
#else
	lbeentry.st_ssl = false;
#endif

#ifdef ENABLE_GSS
	if (MyProcPort && MyProcPort->gss != NULL)
	{
		const char *princ = be_gssapi_get_princ(MyProcPort);

		lbeentry.st_gss = true;
		lgssstatus.gss_auth = be_gssapi_get_auth(MyProcPort);
		lgssstatus.gss_enc = be_gssapi_get_enc(MyProcPort);
		lgssstatus.gss_delegation = be_gssapi_get_delegation(MyProcPort);
		if (princ)
			strlcpy(lgssstatus.gss_princ, princ, NAMEDATALEN);
	}
	else
	{
		lbeentry.st_gss = false;
	}
#else
	lbeentry.st_gss = false;
#endif

	lbeentry.st_state = STATE_UNDEFINED;
	lbeentry.st_progress_command = PROGRESS_COMMAND_INVALID;
	lbeentry.st_progress_command_target = InvalidOid;
	lbeentry.st_query_id = UINT64CONST(0);

	/*
	 * 为节省开销，这里不清零 st_progress_param；在
	 * st_progress_command 被设为 PROGRESS_COMMAND_INVALID 以外的
	 * 值前无人应检查它。
	 */

	/*
	 * 我们即将进入填充共享内存状态条目的临界区。我们
	 * 遵循在前后各递增 st_changecount 的协议，并确保
	 * 之后该值为偶数。这里使用 volatile 指针，以确保
	 * 编译器不会自作聪明。
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(vbeentry);

	/* 确保把相同的 st_changecount memcpy 回去 */
	lbeentry.st_changecount = vbeentry->st_changecount;

	memcpy(unvolatize(PgBackendStatus *, vbeentry),
		   &lbeentry,
		   sizeof(PgBackendStatus));

	/*
	 * 可用 lbeentry 中的指针写行外字符串与结构体，
	 * 这省去了一些去 volatile 化的繁琐处理。
	 */
	lbeentry.st_appname[0] = '\0';
	if (MyProcPort && MyProcPort->remote_hostname)
		strlcpy(lbeentry.st_clienthostname, MyProcPort->remote_hostname,
				NAMEDATALEN);
	else
		lbeentry.st_clienthostname[0] = '\0';
	lbeentry.st_activity_raw[0] = '\0';
	/* 并确保每个字符串区域末字节始终为 0 */
	lbeentry.st_appname[NAMEDATALEN - 1] = '\0';
	lbeentry.st_clienthostname[NAMEDATALEN - 1] = '\0';
	lbeentry.st_activity_raw[pgstat_track_activity_query_size - 1] = '\0';

#ifdef USE_SSL
	memcpy(lbeentry.st_sslstatus, &lsslstatus, sizeof(PgBackendSSLStatus));
#endif
#ifdef ENABLE_GSS
	memcpy(lbeentry.st_gssstatus, &lgssstatus, sizeof(PgBackendGSSStatus));
#endif

	PGSTAT_END_WRITE_ACTIVITY(vbeentry);

	/* 将应用名更新为当前 GUC 设置 */
	if (application_name)
		pgstat_report_appname(application_name);
}

/*
 * 清除其在 PgBackendStatus 数组的条目。
 */
static void
pgstat_beshutdown_hook(int code, Datum arg)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	/*
	 * 清除自己的状态条目，遵循在前后各递增
	 * st_changecount 的协议。这里使用 volatile 指针，
	 * 确保编译器不自作聪明。
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	beentry->st_procpid = 0;	/* 标记无效 */

	PGSTAT_END_WRITE_ACTIVITY(beentry);

	/* 函数可经 MyBEEntry 检查 backend_status.c 是否已就绪 */
	MyBEEntry = NULL;
}

/*
 * 丢弃当前事务中收集的所有数据。任何后续请求都会
 * 导致读取新的快照。
 *
 * 在事务提交或中止期间也会调用本函数，以丢弃不再
 * 需要的快照。
 */
void
pgstat_clear_backend_activity_snapshot(void)
{
	/* 释放内存（若分配过） */
	if (backendStatusSnapContext)
	{
		MemoryContextDelete(backendStatusSnapContext);
		backendStatusSnapContext = NULL;
	}

	/* 重置变量 */
	localBackendStatusTable = NULL;
	localNumBackends = 0;
}

static void
pgstat_setup_backend_status_context(void)
{
	if (!backendStatusSnapContext)
		backendStatusSnapContext = AllocSetContextCreate(TopMemoryContext,
														 "Backend Status Snapshot",
														 ALLOCSET_SMALL_SIZES);
}


/* ----------
 * pgstat_report_activity() -
 *
 *	由 tcop/postgres.c 调用，用于报告后端当前实际在做什么
 *	（注意某些情况下 cmd_str 为 NULL）
 *
 * 对状态条目的所有更新都遵循在前后各递增
 * st_changecount 的协议。这里使用 volatile 指针，
 * 以确保编译器不会自作聪明。
 * ----------
 */
void
pgstat_report_activity(BackendState state, const char *cmd_str)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	TimestampTz start_timestamp;
	TimestampTz current_timestamp;
	int			len = 0;

	TRACE_POSTGRESQL_STATEMENT_STATUS(cmd_str);

	if (!beentry)
		return;

	if (!pgstat_track_activities)
	{
		if (beentry->st_state != STATE_DISABLED)
		{
			volatile PGPROC *proc = MyProc;

			/*
			 * track_activities 已禁用，但上次报告的是
			 * 非禁用状态。作为最后一次更新，改变状态并
			 * 清空不再更新的字段。
			 */
			PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
			beentry->st_state = STATE_DISABLED;
			beentry->st_state_start_timestamp = 0;
			beentry->st_activity_raw[0] = '\0';
			beentry->st_activity_start_timestamp = 0;
			/* st_xact_start_timestamp 与 wait_event_info 也被禁用 */
			beentry->st_xact_start_timestamp = 0;
			beentry->st_query_id = UINT64CONST(0);
			proc->wait_event_info = 0;
			PGSTAT_END_WRITE_ACTIVITY(beentry);
		}
		return;
	}

	/*
	 * 为尽量缩短修改条目的时间，并避免临界区内
	 * 出错的风险，先取好全部所需数据。
	 */
	start_timestamp = GetCurrentStatementStartTimestamp();
	if (cmd_str != NULL)
	{
		/*
		 * 计算待存字符串的长度时不考虑多字节
		 * 字符。出于速度考虑，修正留到读取时进行，
		 * 而非写入时计算。
		 */
		len = Min(strlen(cmd_str), pgstat_track_activity_query_size - 1);
	}
	current_timestamp = GetCurrentTimestamp();

	/*
	 * 若状态已从 "active" 或 "idle in transaction" 改变，
	 * 则计算其时长。
	 */
	if ((beentry->st_state == STATE_RUNNING ||
		 beentry->st_state == STATE_FASTPATH ||
		 beentry->st_state == STATE_IDLEINTRANSACTION ||
		 beentry->st_state == STATE_IDLEINTRANSACTION_ABORTED) &&
		state != beentry->st_state)
	{
		long		secs;
		int			usecs;

		TimestampDifference(beentry->st_state_start_timestamp,
							current_timestamp,
							&secs, &usecs);

		if (beentry->st_state == STATE_RUNNING ||
			beentry->st_state == STATE_FASTPATH)
			pgstat_count_conn_active_time((PgStat_Counter) secs * 1000000 + usecs);
		else
			pgstat_count_conn_txn_idle_time((PgStat_Counter) secs * 1000000 + usecs);
	}

	/*
	 * 现在更新状态条目
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	beentry->st_state = state;
	beentry->st_state_start_timestamp = current_timestamp;

	/*
	 * 若新查询已开始，我们重置查询标识符，因为它要到
	 * 解析分析后得知，以免报告上一个查询的标识
	 * 符。
	 */
	if (state == STATE_RUNNING)
		beentry->st_query_id = UINT64CONST(0);

	if (cmd_str != NULL)
	{
		memcpy((char *) beentry->st_activity_raw, cmd_str, len);
		beentry->st_activity_raw[len] = '\0';
		beentry->st_activity_start_timestamp = start_timestamp;
	}

	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/* --------
 * pgstat_report_query_id() -
 *
 * 用于更新顶层查询标识符。
 * --------
 */
void
pgstat_report_query_id(uint64 query_id, bool force)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	/*
	 * 若 track_activities 已禁用，st_query_id 应已重置
	 *
	 */
	if (!beentry || !pgstat_track_activities)
		return;

	/*
	 * 我们只报告顶层查询标识符。存储的 query_id 会在
	 * 后端调用 pgstat_report_activity(STATE_RUNNING) 时，
	 * 或在以 force 标志显式调用本函数时重置。若保存的
	 * 查询标识符非零，说明它不是顶层命令，除非是
	 * 显式调用以重置标识符，否则忽略所提供的
	 * 标识符。
	 */
	if (beentry->st_query_id != 0 && !force)
		return;

	/*
	 * 更新自己的状态条目，遵循在前后各递增
	 * st_changecount 的协议。这里使用 volatile 指针，
	 * 以确保编译器不会自作聪明。
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_query_id = query_id;
	PGSTAT_END_WRITE_ACTIVITY(beentry);
}


/* ----------
 * pgstat_report_appname() -
 *
 *	用于更新我们的应用名。
 * ----------
 */
void
pgstat_report_appname(const char *appname)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			len;

	if (!beentry)
		return;

	/* 若 GUC 已尽其职这本无必要，但求稳妥 */
	len = pg_mbcliplen(appname, strlen(appname), NAMEDATALEN - 1);

	/*
	 * 更新自己的状态条目，遵循在前后各递增
	 * st_changecount 的协议。这里使用 volatile 指针，
	 * 以确保编译器不会自作聪明。
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	memcpy((char *) beentry->st_appname, appname, len);
	beentry->st_appname[len] = '\0';

	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/*
 * 将当前事务开始时间戳报告为指定值。
 * 零表示当前没有活动事务。
 */
void
pgstat_report_xact_timestamp(TimestampTz tstamp)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!pgstat_track_activities || !beentry)
		return;

	/*
	 * 更新自己的状态条目，遵循在前后各递增
	 * st_changecount 的协议。这里使用 volatile 指针，
	 * 以确保编译器不会自作聪明。
	 */
	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	beentry->st_xact_start_timestamp = tstamp;

	PGSTAT_END_WRITE_ACTIVITY(beentry);
}

/* ----------
 * pgstat_read_current_status() -
 *
 *	将 PgBackendStatus 数组的当前内容复制到本地内存，
 *	若本事务中尚未这样做。
 * ----------
 */
static void
pgstat_read_current_status(void)
{
	volatile PgBackendStatus *beentry;
	LocalPgBackendStatus *localtable;
	LocalPgBackendStatus *localentry;
	char	   *localappname,
			   *localclienthostname,
			   *localactivity;
#ifdef USE_SSL
	PgBackendSSLStatus *localsslstatus;
#endif
#ifdef ENABLE_GSS
	PgBackendGSSStatus *localgssstatus;
#endif
	int			i;

	if (localBackendStatusTable)
		return;					/* 已做过 */

	pgstat_setup_backend_status_context();

	/*
	 * 为状态数据本地副本分配存储。可以假定这些请求
	 * 均不会溢出 size_t，因为共享内存设置期间已经用
	 * mul_size 算出了相同的值。但如果
	 * pgstat_track_activity_query_size 与 max_connections 取了
	 * 大概离谱的值，localactivity 缓冲区可能超过 1GB，
	 * 那个用 "huge" 分配。
	 */
	localtable = (LocalPgBackendStatus *)
		MemoryContextAlloc(backendStatusSnapContext,
						   sizeof(LocalPgBackendStatus) * NumBackendStatSlots);
	localappname = (char *)
		MemoryContextAlloc(backendStatusSnapContext,
						   NAMEDATALEN * NumBackendStatSlots);
	localclienthostname = (char *)
		MemoryContextAlloc(backendStatusSnapContext,
						   NAMEDATALEN * NumBackendStatSlots);
	localactivity = (char *)
		MemoryContextAllocHuge(backendStatusSnapContext,
							   (Size) pgstat_track_activity_query_size *
							   (Size) NumBackendStatSlots);
#ifdef USE_SSL
	localsslstatus = (PgBackendSSLStatus *)
		MemoryContextAlloc(backendStatusSnapContext,
						   sizeof(PgBackendSSLStatus) * NumBackendStatSlots);
#endif
#ifdef ENABLE_GSS
	localgssstatus = (PgBackendGSSStatus *)
		MemoryContextAlloc(backendStatusSnapContext,
						   sizeof(PgBackendGSSStatus) * NumBackendStatSlots);
#endif

	localNumBackends = 0;

	beentry = BackendStatusArray;
	localentry = localtable;
	for (i = 1; i <= NumBackendStatSlots; i++)
	{
		/*
		 * 遵循如下协议：若复制条目时 st_changecount
		 * 发生变化或值为奇数则重试。（需要检查奇数是
		 * 为了覆盖这样一种情形：源后端正处于两次递增之
		 * 间时我们恰能完整复制条目。）这里使用 volatile
		 * 指针，以确保编译器不会自作聪明。
		 */
		for (;;)
		{
			int			before_changecount;
			int			after_changecount;

			pgstat_begin_read_activity(beentry, before_changecount);

			localentry->backendStatus.st_procpid = beentry->st_procpid;
			/* 条目未在使用则跳过全部数据复制工作 */
			if (localentry->backendStatus.st_procpid > 0)
			{
				memcpy(&localentry->backendStatus, unvolatize(PgBackendStatus *, beentry), sizeof(PgBackendStatus));

				/*
				 * 对 PgBackendStatus 中每个指针型字段，复制
				 * 指向的数据，再将指针字段的本地副本调整
				 * 为指向数据的本地副本。
				 *
				 * 即使字符串被并发修改，strcpy 也是安全的，
				 * 因为缓冲区末尾总是有一个 \0。
				 */
				strcpy(localappname, (char *) beentry->st_appname);
				localentry->backendStatus.st_appname = localappname;
				strcpy(localclienthostname, (char *) beentry->st_clienthostname);
				localentry->backendStatus.st_clienthostname = localclienthostname;
				strcpy(localactivity, (char *) beentry->st_activity_raw);
				localentry->backendStatus.st_activity_raw = localactivity;
#ifdef USE_SSL
				if (beentry->st_ssl)
				{
					memcpy(localsslstatus, beentry->st_sslstatus, sizeof(PgBackendSSLStatus));
					localentry->backendStatus.st_sslstatus = localsslstatus;
				}
#endif
#ifdef ENABLE_GSS
				if (beentry->st_gss)
				{
					memcpy(localgssstatus, beentry->st_gssstatus, sizeof(PgBackendGSSStatus));
					localentry->backendStatus.st_gssstatus = localgssstatus;
				}
#endif
			}

			pgstat_end_read_activity(beentry, after_changecount);

			if (pgstat_read_activity_complete(before_changecount,
											  after_changecount))
				break;

			/* 确保卡住时能跳出循环... */
			CHECK_FOR_INTERRUPTS();
		}

		/* 只有有效条目才会被收进本地数组 */
		if (localentry->backendStatus.st_procpid > 0)
		{
			/*
			 * BackendStatusArray 索引恰为源后端的
			 * BackendId。注意这意味着 localBackendStatusTable
			 * 按 backend_id 排序。pgstat_get_beentry_by_backend_id()
			 * 依赖此点。
			 */
			localentry->backend_id = i;
			BackendIdGetTransactionIds(i,
									   &localentry->backend_xid,
									   &localentry->backend_xmin,
									   &localentry->backend_subxact_count,
									   &localentry->backend_subxact_overflowed);

			localentry++;
			localappname += NAMEDATALEN;
			localclienthostname += NAMEDATALEN;
			localactivity += pgstat_track_activity_query_size;
#ifdef USE_SSL
			localsslstatus++;
#endif
#ifdef ENABLE_GSS
			localgssstatus++;
#endif
			localNumBackends++;
		}

		beentry++;
	}

	/* 仅在有效表构建完成后才设置该指针 */
	localBackendStatusTable = localtable;
}


/* ----------
 * pgstat_get_backend_current_activity() -
 *
 *	返回表示具有指定 PID 的后端当前活动的字符串。
 *	本函数直接查看 BackendStatusArray，因此提供的
 *	信息始终是最新的，与本事务在状态数组上的快照
 *	有多旧无关。
 *
 *	调用方有责任只对状态在结果使用期间预期保持稳定
 *	的后端调用本函数。目前唯一的用途是死锁报告，
 *	在那里我们可以预期目标后端已被某个锁阻塞。
 *	（存在边角情形：查看时其等待可能被中止，
 *	但最坏后果也只是返回指向已变动字符串
 *	的指针，因此
 *	我们不必太过担心。）
 *
 *	注：特殊情形返回值与 pg_stat_get_backend_activity 相同。
 * ----------
 */
const char *
pgstat_get_backend_current_activity(int pid, bool checkUser)
{
	PgBackendStatus *beentry;
	int			i;

	beentry = BackendStatusArray;
	for (i = 1; i <= MaxBackends; i++)
	{
		/*
		 * 虽然预期目标后端的条目是稳定的，但这并不
		 * 代表别人的也稳定。为避免认错后端，在
		 * 检查是否匹配所需 PID 期间，须遵循如下协议：
		 * 若期间 st_changecount 变化或值为奇数则重试
		 * （这可能多余，因为 int 的读取或存储几乎
		 * 肯定是原子操作，但我们还是稳妥为好。）这里
		 * 使用 volatile 指针，以确保编译器不会
		 * 自作聪明。
		 */
		volatile PgBackendStatus *vbeentry = beentry;
		bool		found;

		for (;;)
		{
			int			before_changecount;
			int			after_changecount;

			pgstat_begin_read_activity(vbeentry, before_changecount);

			found = (vbeentry->st_procpid == pid);

			pgstat_end_read_activity(vbeentry, after_changecount);

			if (pgstat_read_activity_complete(before_changecount,
											  after_changecount))
				break;

			/* 确保卡住时能跳出循环... */
			CHECK_FOR_INTERRUPTS();
		}

		if (found)
		{
			/* 现在可以安全使用非 volatile 指针了 */
			if (checkUser && !superuser() && beentry->st_userid != GetUserId())
				return "<insufficient privilege>";
			else if (*(beentry->st_activity_raw) == '\0')
				return "<command string not enabled>";
			else
			{
				/* 这会泄漏一点内存，但看来可以接受 */
				return pgstat_clip_activity(beentry->st_activity_raw);
			}
		}

		beentry++;
	}

	/* 走到这里说明调用方有误 ... */
	return "<backend information not available>";
}

/* ----------
 * pgstat_get_crashed_backend_activity() -
 *
 *	返回表示具有指定 PID 的后端当前活动的字符串。与
 *	上面的函数类似，但读取共享内存时假定它可能已损
 *	坏。成功时把字符串复制进 "buffer" 参数并返回
 *	该指针；失败时返回
 *	NULL。
 *
 *	本函数仅打算由 postmaster 用来报告导致某个后端崩溃
 *	的查询。特别地，在访问 BackendStatusArray 时，
 *	不做任何努力去遵循正确的并发协议。但没
 *	关系，最坏情况我们也只会返回一条损坏的消息。
 *	我们还必须小心不要触发 ereport(ERROR)。
 * ----------
 */
const char *
pgstat_get_crashed_backend_activity(int pid, char *buffer, int buflen)
{
	volatile PgBackendStatus *beentry;
	int			i;

	beentry = BackendStatusArray;

	/*
	 * 共享内存建立之前我们大概不该走到这里，但为
	 * 稳妥。
	 */
	if (beentry == NULL || BackendActivityBuffer == NULL)
		return NULL;

	for (i = 1; i <= MaxBackends; i++)
	{
		if (beentry->st_procpid == pid)
		{
			/* 只读取一次指针，使其在校验之后不会变化 */
			const char *activity = beentry->st_activity_raw;
			const char *activity_last;

			/*
			 * 在核实活动字符串落在 BackendActivityBuffer
			 * 内之前不得访问它。为确保字符串连其结尾
			 * 都包含在缓冲区内，从缓冲区大小减去一
			 * 个活动长度。
			 */
			activity_last = BackendActivityBuffer + BackendActivityBufferSize
				- pgstat_track_activity_query_size;

			if (activity < BackendActivityBuffer ||
				activity > activity_last)
				return NULL;

			/* 没有可用字符串时报告无意义 */
			if (activity[0] == '\0')
				return NULL;

			/*
			 * 只复制 ASCII 安全的字符，以免报告消息时遇到
			 * 编码问题；并务必不要越过内存末尾。由于只
			 * 报告的都是 ASCII 字符，似无必要执行多字节
			 * 感知的裁剪。
			 */
			ascii_safe_strlcpy(buffer, activity,
							   Min(buflen, pgstat_track_activity_query_size));

			return buffer;
		}

		beentry++;
	}

	/* 未找到 PID */
	return NULL;
}

/* ----------
 * pgstat_get_my_query_id() -
 *
 * 返回当前后端的查询标识符。
 */
uint64
pgstat_get_my_query_id(void)
{
	if (!MyBEEntry)
		return 0;

	/*
	 * 这里的 pgstat_begin_read_activity /
	 * pgstat_end_read_activity 不需要锁，因为它只
	 * 从 pg_stat_get_activity（其已受保护）调用，或从同
	 * 一后端调用，这意味着不会有并发写入。
	 */
	return MyBEEntry->st_query_id;
}

/* ----------
 * cmp_lbestatus
 *
 *	用于对 LocalPgBackendStatus 数组做 bsearch() 的比较函数。
 *	使用 backend_id 字段来比较各参数。
 * ----------
 */
static int
cmp_lbestatus(const void *a, const void *b)
{
	const LocalPgBackendStatus *lbestatus1 = (const LocalPgBackendStatus *) a;
	const LocalPgBackendStatus *lbestatus2 = (const LocalPgBackendStatus *) b;

	return lbestatus1->backend_id - lbestatus2->backend_id;
}

/* ----------
 * pgstat_get_beentry_by_backend_id() -
 *
 *	供 SQL 可调用的 pgstat* 函数的辅助函数。返回
 *	某后端当前活动条目的本地副本，若给定 beid
 *	不能标识任何已知会话则返回 NULL。
 *
 *	beid 参数是目标会话的 BackendId
 *	（注意这与 pgstat_get_local_beentry_by_index() 不同）
 *
 *	注意：调用方有责任检查用户是否可查看此信息
 *	（尤其是查询字符串）。
 * ----------
 */
PgBackendStatus *
pgstat_get_beentry_by_backend_id(BackendId beid)
{
	LocalPgBackendStatus *ret = pgstat_get_local_beentry_by_backend_id(beid);

	if (ret)
		return &ret->backendStatus;

	return NULL;
}


/* ----------
 * pgstat_get_local_beentry_by_backend_id() -
 *
 *	类似 pgstat_get_beentry_by_backend_id()，但带有本地计算的
 *	附加项（如 xid 与 xmin 值）。
 *
 *	beid 参数是目标会话的 BackendId
 *	（注意这与 pgstat_get_local_beentry_by_index() 不同）
 *
 *	注意：调用方有责任检查用户是否被允许查看此信息
 *	（尤其是查询字符串）。
 * ----------
 */
LocalPgBackendStatus *
pgstat_get_local_beentry_by_backend_id(BackendId beid)
{
	LocalPgBackendStatus key;

	pgstat_read_current_status();

	/*
	 * 由于 localBackendStatusTable 按 backend_id 排序，我们
	 * 可用 bsearch() 高效搜索它。
	 */
	key.backend_id = beid;
	return bsearch(&key, localBackendStatusTable, localNumBackends,
				   sizeof(LocalPgBackendStatus), cmp_lbestatus);
}


/* ----------
 * pgstat_get_local_beentry_by_index() -
 *
 *	类似 pgstat_get_beentry_by_backend_id()，但带有本地计算的
 *	附加项（如 xid 与 xmin 值）。
 *
 *	idx 参数是 localBackendStatusTable 中基于 1 的索引
 *	（注意这与 pgstat_get_beentry_by_backend_id() 不同）
 *	若参数越界则返回 NULL（当前没有调用方会这样做）。
 *
 *	注意：调用方有责任检查用户是否可查看此信息
 *	（尤其是查询字符串）。
 * ----------
 */
LocalPgBackendStatus *
pgstat_get_local_beentry_by_index(int idx)
{
	pgstat_read_current_status();

	if (idx < 1 || idx > localNumBackends)
		return NULL;

	return &localBackendStatusTable[idx - 1];
}


/* ----------
 * pgstat_fetch_stat_numbackends() -
 *
 *	供 SQL 可调用的 pgstat* 函数的辅助函数。返回
 *	localBackendStatusTable 中已知会话的数量，即
 *	传给 pgstat_get_local_beentry_by_index() 的最大基于 1 的索引。
 * ----------
 */
int
pgstat_fetch_stat_numbackends(void)
{
	pgstat_read_current_status();

	return localNumBackends;
}

/*
 * 把可能被不安全截断的活动字符串（见
 * PgBackendStatus.st_activity_raw 的文档）转换成正确截断的
 *
 *
 * 返回的字符串在调用方的内存上下文中分配，并可
 * 释放
 */
char *
pgstat_clip_activity(const char *raw_activity)
{
	char	   *activity;
	int			rawlen;
	int			cliplen;

	/*
	 * 某些调用方，如 pgstat_get_backend_current_activity()，
	 * 不保证缓冲区不会被并发修改。我们仍设法确保缓
	 * 冲区始终以 NUL 字节结尾，但我们仍要对字符串长度
	 * 保持多疑。在这类情形下，底层缓冲区
	 * 的大小保证为 pgstat_track_activity_query_size
	 * 大。
	 */
	activity = pnstrdup(raw_activity, pgstat_track_activity_query_size - 1);

	/* 现在双重保证以 NUL 结尾 */
	rawlen = strlen(activity);

	/*
	 * 所有受支持的服务器编码都可以从多字节字符的
	 * 首字节确定其长度（对客户端编码并非如此，
	 * 见 GB18030）。st_activity 始终以服务器编码存储，
	 * 这使我们可以执行多字节感知的截断，
	 * 即使该字符串早前是在多字节字符中间被截断
	 * 的。
	 */
	cliplen = pg_mbcliplen(activity, rawlen,
						   pgstat_track_activity_query_size - 1);

	activity[cliplen] = '\0';

	return activity;
}
