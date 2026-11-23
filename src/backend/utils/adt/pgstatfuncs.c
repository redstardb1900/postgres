/*-------------------------------------------------------------------------
 *
 * pgstatfuncs.c
 *	  访问各类统计数据的函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pgstatfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "catalog/catalog.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker_internals.h"
#include "postmaster/postmaster.h"
#include "replication/logicallauncher.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inet.h"
#include "utils/timestamp.h"

#define UINT32_ACCESS_ONCE(var)		 ((uint32)(*((volatile uint32 *)&(var))))

#define HAS_PGSTAT_PERMISSIONS(role)	 (has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_STATS) || has_privs_of_role(GetUserId(), role))

#define PG_STAT_GET_RELENTRY_INT64(stat)						\
Datum															\
CppConcat(pg_stat_get_,stat)(PG_FUNCTION_ARGS)					\
{																\
	Oid			relid = PG_GETARG_OID(0);						\
	int64		result;											\
	PgStat_StatTabEntry *tabentry;								\
																\
	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)	\
		result = 0;												\
	else														\
		result = (int64) (tabentry->stat);						\
																\
	PG_RETURN_INT64(result);									\
}

/* pg_stat_get_analyze_count */
PG_STAT_GET_RELENTRY_INT64(analyze_count)

/* pg_stat_get_autoanalyze_count */
PG_STAT_GET_RELENTRY_INT64(autoanalyze_count)

/* pg_stat_get_autovacuum_count */
PG_STAT_GET_RELENTRY_INT64(autovacuum_count)

/* pg_stat_get_blocks_fetched */
PG_STAT_GET_RELENTRY_INT64(blocks_fetched)

/* pg_stat_get_blocks_hit */
PG_STAT_GET_RELENTRY_INT64(blocks_hit)

/* pg_stat_get_dead_tuples */
PG_STAT_GET_RELENTRY_INT64(dead_tuples)

/* pg_stat_get_ins_since_vacuum */
PG_STAT_GET_RELENTRY_INT64(ins_since_vacuum)

/* pg_stat_get_live_tuples */
PG_STAT_GET_RELENTRY_INT64(live_tuples)

/* pg_stat_get_mod_since_analyze */
PG_STAT_GET_RELENTRY_INT64(mod_since_analyze)

/* pg_stat_get_numscans */
PG_STAT_GET_RELENTRY_INT64(numscans)

/* pg_stat_get_tuples_deleted */
PG_STAT_GET_RELENTRY_INT64(tuples_deleted)

/* pg_stat_get_tuples_fetched */
PG_STAT_GET_RELENTRY_INT64(tuples_fetched)

/* pg_stat_get_tuples_hot_updated */
PG_STAT_GET_RELENTRY_INT64(tuples_hot_updated)

/* pg_stat_get_tuples_newpage_updated */
PG_STAT_GET_RELENTRY_INT64(tuples_newpage_updated)

/* pg_stat_get_tuples_inserted */
PG_STAT_GET_RELENTRY_INT64(tuples_inserted)

/* pg_stat_get_tuples_returned */
PG_STAT_GET_RELENTRY_INT64(tuples_returned)

/* pg_stat_get_tuples_updated */
PG_STAT_GET_RELENTRY_INT64(tuples_updated)

/* pg_stat_get_vacuum_count */
PG_STAT_GET_RELENTRY_INT64(vacuum_count)

#define PG_STAT_GET_RELENTRY_TIMESTAMPTZ(stat)					\
Datum															\
CppConcat(pg_stat_get_,stat)(PG_FUNCTION_ARGS)					\
{																\
	Oid			relid = PG_GETARG_OID(0);						\
	TimestampTz result;											\
	PgStat_StatTabEntry *tabentry;								\
																\
	if ((tabentry = pgstat_fetch_stat_tabentry(relid)) == NULL)	\
		result = 0;												\
	else														\
		result = tabentry->stat;								\
																\
	if (result == 0)											\
		PG_RETURN_NULL();										\
	else														\
		PG_RETURN_TIMESTAMPTZ(result);							\
}

/* pg_stat_get_last_analyze_time */
PG_STAT_GET_RELENTRY_TIMESTAMPTZ(last_analyze_time)

/* pg_stat_get_last_autoanalyze_time */
PG_STAT_GET_RELENTRY_TIMESTAMPTZ(last_autoanalyze_time)

/* pg_stat_get_last_autovacuum_time */
PG_STAT_GET_RELENTRY_TIMESTAMPTZ(last_autovacuum_time)

/* pg_stat_get_last_vacuum_time */
PG_STAT_GET_RELENTRY_TIMESTAMPTZ(last_vacuum_time)

/* pg_stat_get_lastscan */
PG_STAT_GET_RELENTRY_TIMESTAMPTZ(lastscan)

Datum
pg_stat_get_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_StatFuncEntry *funcentry;

	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->numcalls);
}

/* 把计数器从微秒转为毫秒以便显示 */
#define PG_STAT_GET_FUNCENTRY_FLOAT8_MS(stat)						\
Datum																\
CppConcat(pg_stat_get_function_,stat)(PG_FUNCTION_ARGS)				\
{																	\
	Oid			funcid = PG_GETARG_OID(0);							\
	double		result;												\
	PgStat_StatFuncEntry *funcentry;								\
																	\
	if ((funcentry = pgstat_fetch_stat_funcentry(funcid)) == NULL)	\
		PG_RETURN_NULL();											\
	result = ((double) funcentry->stat) / 1000.0;					\
	PG_RETURN_FLOAT8(result);										\
}

/* pg_stat_get_function_total_time */
PG_STAT_GET_FUNCENTRY_FLOAT8_MS(total_time)

/* pg_stat_get_function_self_time */
PG_STAT_GET_FUNCENTRY_FLOAT8_MS(self_time)

Datum
pg_stat_get_backend_idset(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int		   *fctx;

	/* 仅在函数首次调用时做的事 */
	if (SRF_IS_FIRSTCALL())
	{
		/* 创建跨调用持久化的函数上下文 */
		funcctx = SRF_FIRSTCALL_INIT();

		fctx = MemoryContextAlloc(funcctx->multi_call_memory_ctx,
								  sizeof(int));
		funcctx->user_fctx = fctx;

		fctx[0] = 0;
	}

	/* 函数每次调用都做的事 */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	fctx[0] += 1;

	/*
	 * 每次循环都重查 pgstat_fetch_stat_numbackends(),以防
	 * 本地状态自开始后已刷新。未刷新时开销也
	 * 足够便宜。若确有刷新,可能漏掉或重复某些
	 * 后端 ID,但可接受只要不崩溃。
	 * (在此类查询中途刷新本就用法有问题,
	 * 因已返回的后端 ID 可能不再
	 * 指向现存会话。)
	 */
	if (fctx[0] <= pgstat_fetch_stat_numbackends())
	{
		/* 还有剩余要发送时执行 */
		LocalPgBackendStatus *local_beentry = pgstat_get_local_beentry_by_index(fctx[0]);

		SRF_RETURN_NEXT(funcctx, Int32GetDatum(local_beentry->backend_id));
	}
	else
	{
		/* 没有剩余时执行 */
		SRF_RETURN_DONE(funcctx);
	}
}

/*
 * 返回指定命令的命令进度信息。
 */
Datum
pg_stat_get_progress_info(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_PROGRESS_COLS	PGSTAT_NUM_PROGRESS_PARAM + 3
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	char	   *cmd = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ProgressCommandType cmdtype;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	/* 把命令名翻译为命令类型码。 */
	if (pg_strcasecmp(cmd, "VACUUM") == 0)
		cmdtype = PROGRESS_COMMAND_VACUUM;
	else if (pg_strcasecmp(cmd, "ANALYZE") == 0)
		cmdtype = PROGRESS_COMMAND_ANALYZE;
	else if (pg_strcasecmp(cmd, "CLUSTER") == 0)
		cmdtype = PROGRESS_COMMAND_CLUSTER;
	else if (pg_strcasecmp(cmd, "CREATE INDEX") == 0)
		cmdtype = PROGRESS_COMMAND_CREATE_INDEX;
	else if (pg_strcasecmp(cmd, "BASEBACKUP") == 0)
		cmdtype = PROGRESS_COMMAND_BASEBACKUP;
	else if (pg_strcasecmp(cmd, "COPY") == 0)
		cmdtype = PROGRESS_COMMAND_COPY;
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid command name: \"%s\"", cmd)));

	InitMaterializedSRF(fcinfo, 0);

	/* 1 起算 */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		Datum		values[PG_STAT_GET_PROGRESS_COLS] = {0};
		bool		nulls[PG_STAT_GET_PROGRESS_COLS] = {0};
		int			i;

		local_beentry = pgstat_get_local_beentry_by_index(curr_backend);
		beentry = &local_beentry->backendStatus;

		/*
		 * 仅汇报执行给定命令的后端。
		 * 
		 */
		if (beentry->st_progress_command != cmdtype)
			continue;

		/* 所有调用者均可获的值 */
		values[0] = Int32GetDatum(beentry->st_procpid);
		values[1] = ObjectIdGetDatum(beentry->st_databaseid);

		/* 含 relid 的其余值仅向角色成员显示 */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			values[2] = ObjectIdGetDatum(beentry->st_progress_command_target);
			for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; i++)
				values[i + 3] = Int64GetDatum(beentry->st_progress_param[i]);
		}
		else
		{
			nulls[2] = true;
			for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; i++)
				nulls[i + 3] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

/*
 * 返回 PG 后端的活动。
 */
Datum
pg_stat_get_activity(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_ACTIVITY_COLS	31
	int			num_backends = pgstat_fetch_stat_numbackends();
	int			curr_backend;
	int			pid = PG_ARGISNULL(0) ? -1 : PG_GETARG_INT32(0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);

	/* 1 起算 */
	for (curr_backend = 1; curr_backend <= num_backends; curr_backend++)
	{
		/* 每行 */
		Datum		values[PG_STAT_GET_ACTIVITY_COLS] = {0};
		bool		nulls[PG_STAT_GET_ACTIVITY_COLS] = {0};
		LocalPgBackendStatus *local_beentry;
		PgBackendStatus *beentry;
		PGPROC	   *proc;
		const char *wait_event_type = NULL;
		const char *wait_event = NULL;

		/* 取列表中下一个 */
		local_beentry = pgstat_get_local_beentry_by_index(curr_backend);
		beentry = &local_beentry->backendStatus;

		/* 若查找特定 PID,忽略所有其他 */
		if (pid != -1 && beentry->st_procpid != pid)
			continue;

		/* 所有调用者可获的值 */
		if (beentry->st_databaseid != InvalidOid)
			values[0] = ObjectIdGetDatum(beentry->st_databaseid);
		else
			nulls[0] = true;

		values[1] = Int32GetDatum(beentry->st_procpid);

		if (beentry->st_userid != InvalidOid)
			values[2] = ObjectIdGetDatum(beentry->st_userid);
		else
			nulls[2] = true;

		if (beentry->st_appname)
			values[3] = CStringGetTextDatum(beentry->st_appname);
		else
			nulls[3] = true;

		if (TransactionIdIsValid(local_beentry->backend_xid))
			values[15] = TransactionIdGetDatum(local_beentry->backend_xid);
		else
			nulls[15] = true;

		if (TransactionIdIsValid(local_beentry->backend_xmin))
			values[16] = TransactionIdGetDatum(local_beentry->backend_xmin);
		else
			nulls[16] = true;

		/* 仅角色成员或 pg_read_all_stats 可获的值 */
		if (HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		{
			SockAddr	zero_clientaddr;
			char	   *clipped_activity;

			switch (beentry->st_state)
			{
				case STATE_IDLE:
					values[4] = CStringGetTextDatum("idle");
					break;
				case STATE_RUNNING:
					values[4] = CStringGetTextDatum("active");
					break;
				case STATE_IDLEINTRANSACTION:
					values[4] = CStringGetTextDatum("idle in transaction");
					break;
				case STATE_FASTPATH:
					values[4] = CStringGetTextDatum("fastpath function call");
					break;
				case STATE_IDLEINTRANSACTION_ABORTED:
					values[4] = CStringGetTextDatum("idle in transaction (aborted)");
					break;
				case STATE_DISABLED:
					values[4] = CStringGetTextDatum("disabled");
					break;
				case STATE_UNDEFINED:
					nulls[4] = true;
					break;
			}

			clipped_activity = pgstat_clip_activity(beentry->st_activity_raw);
			values[5] = CStringGetTextDatum(clipped_activity);
			pfree(clipped_activity);

			/* leader_pid */
			nulls[29] = true;

			proc = BackendPidGetProc(beentry->st_procpid);

			if (proc == NULL && (beentry->st_backendType != B_BACKEND))
			{
				/*
				 * 对辅助进程,从共享内存中的
				 * AuxiliaryProcs 取进程信息。
				 */
				proc = AuxiliaryPidGetProc(beentry->st_procpid);
			}

			/*
			 * 若取到了 PGPROC 条目,则显示等待事件以及锁
			 * 组领导者或应用领导者信息(若有)。为避免
			 * 额外开销,未持有额外锁,故多行之间
			 * 不保证一致性。
			 */
			if (proc != NULL)
			{
				uint32		raw_wait_event;
				PGPROC	   *leader;

				raw_wait_event = UINT32_ACCESS_ONCE(proc->wait_event_info);
				wait_event_type = pgstat_get_wait_event_type(raw_wait_event);
				wait_event = pgstat_get_wait_event(raw_wait_event);

				leader = proc->lockGroupLeader;

				/*
				 * 仅对活动并行工作进程显示领导者。这会
				 * 让并行组领导者或并行应用工作进程
				 * 领导者的该字段为 NULL。
				 */
				if (leader && leader->pid != beentry->st_procpid)
				{
					values[29] = Int32GetDatum(leader->pid);
					nulls[29] = false;
				}
				else if (beentry->st_backendType == B_BG_WORKER)
				{
					int			leader_pid = GetLeaderApplyWorkerPid(beentry->st_procpid);

					if (leader_pid != InvalidPid)
					{
						values[29] = Int32GetDatum(leader_pid);
						nulls[29] = false;
					}
				}
			}

			if (wait_event_type)
				values[6] = CStringGetTextDatum(wait_event_type);
			else
				nulls[6] = true;

			if (wait_event)
				values[7] = CStringGetTextDatum(wait_event);
			else
				nulls[7] = true;

			/*
			 * 不对 walsender 暴露事务时间;它会
			 * 干扰监控,尤因未及时更新该时间。
			 * 
			 */
			if (beentry->st_xact_start_timestamp != 0 &&
				beentry->st_backendType != B_WAL_SENDER)
				values[8] = TimestampTzGetDatum(beentry->st_xact_start_timestamp);
			else
				nulls[8] = true;

			if (beentry->st_activity_start_timestamp != 0)
				values[9] = TimestampTzGetDatum(beentry->st_activity_start_timestamp);
			else
				nulls[9] = true;

			if (beentry->st_proc_start_timestamp != 0)
				values[10] = TimestampTzGetDatum(beentry->st_proc_start_timestamp);
			else
				nulls[10] = true;

			if (beentry->st_state_start_timestamp != 0)
				values[11] = TimestampTzGetDatum(beentry->st_state_start_timestamp);
			else
				nulls[11] = true;

			/* 客户端地址全零表示未知 */
			memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
			if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
					   sizeof(zero_clientaddr)) == 0)
			{
				nulls[12] = true;
				nulls[13] = true;
				nulls[14] = true;
			}
			else
			{
				if (beentry->st_clientaddr.addr.ss_family == AF_INET ||
					beentry->st_clientaddr.addr.ss_family == AF_INET6)
				{
					char		remote_host[NI_MAXHOST];
					char		remote_port[NI_MAXSERV];
					int			ret;

					remote_host[0] = '\0';
					remote_port[0] = '\0';
					ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
											 beentry->st_clientaddr.salen,
											 remote_host, sizeof(remote_host),
											 remote_port, sizeof(remote_port),
											 NI_NUMERICHOST | NI_NUMERICSERV);
					if (ret == 0)
					{
						clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);
						values[12] = DirectFunctionCall1(inet_in,
														 CStringGetDatum(remote_host));
						if (beentry->st_clienthostname &&
							beentry->st_clienthostname[0])
							values[13] = CStringGetTextDatum(beentry->st_clienthostname);
						else
							nulls[13] = true;
						values[14] = Int32GetDatum(atoi(remote_port));
					}
					else
					{
						nulls[12] = true;
						nulls[13] = true;
						nulls[14] = true;
					}
				}
				else if (beentry->st_clientaddr.addr.ss_family == AF_UNIX)
				{
					/*
					 * Unix 套接字对 host 恒报 NULL、对
					 * port 恒报 -1,故可区分于无权查看
					 * 的连接或出错情况。
					 * 
					 */
					nulls[12] = true;
					nulls[13] = true;
					values[14] = Int32GetDatum(-1);
				}
				else
				{
					/* 未知地址类型,不应发生 */
					nulls[12] = true;
					nulls[13] = true;
					nulls[14] = true;
				}
			}
			/* 加后端类型 */
			if (beentry->st_backendType == B_BG_WORKER)
			{
				const char *bgw_type;

				bgw_type = GetBackgroundWorkerTypeByPid(beentry->st_procpid);
				if (bgw_type)
					values[17] = CStringGetTextDatum(bgw_type);
				else
					nulls[17] = true;
			}
			else
				values[17] =
					CStringGetTextDatum(GetBackendTypeDesc(beentry->st_backendType));

			/* SSL 信息 */
			if (beentry->st_ssl)
			{
				values[18] = BoolGetDatum(true);	/* ssl */
				values[19] = CStringGetTextDatum(beentry->st_sslstatus->ssl_version);
				values[20] = CStringGetTextDatum(beentry->st_sslstatus->ssl_cipher);
				values[21] = Int32GetDatum(beentry->st_sslstatus->ssl_bits);

				if (beentry->st_sslstatus->ssl_client_dn[0])
					values[22] = CStringGetTextDatum(beentry->st_sslstatus->ssl_client_dn);
				else
					nulls[22] = true;

				if (beentry->st_sslstatus->ssl_client_serial[0])
					values[23] = DirectFunctionCall3(numeric_in,
													 CStringGetDatum(beentry->st_sslstatus->ssl_client_serial),
													 ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1));
				else
					nulls[23] = true;

				if (beentry->st_sslstatus->ssl_issuer_dn[0])
					values[24] = CStringGetTextDatum(beentry->st_sslstatus->ssl_issuer_dn);
				else
					nulls[24] = true;
			}
			else
			{
				values[18] = BoolGetDatum(false);	/* ssl */
				nulls[19] = nulls[20] = nulls[21] = nulls[22] = nulls[23] = nulls[24] = true;
			}

			/* GSSAPI 信息 */
			if (beentry->st_gss)
			{
				values[25] = BoolGetDatum(beentry->st_gssstatus->gss_auth); /* gss_auth */
				values[26] = CStringGetTextDatum(beentry->st_gssstatus->gss_princ);
				values[27] = BoolGetDatum(beentry->st_gssstatus->gss_enc);	/* GSS 加密使用中 */
				values[28] = BoolGetDatum(beentry->st_gssstatus->gss_delegation);	/* GSS 凭据
																					 * 已委派 */
			}
			else
			{
				values[25] = BoolGetDatum(false);	/* gss_auth */
				nulls[26] = true;	/* 无 GSS 主体 */
				values[27] = BoolGetDatum(false);	/* GSS 加密未使用
													 *  */
				values[28] = BoolGetDatum(false);	/* GSS 凭据未
													 * 委派 */
			}
			if (beentry->st_query_id == 0)
				nulls[30] = true;
			else
				values[30] = UInt64GetDatum(beentry->st_query_id);
		}
		else
		{
			/* 无权查看此会话的数据 */
			values[5] = CStringGetTextDatum("<insufficient privilege>");
			nulls[4] = true;
			nulls[6] = true;
			nulls[7] = true;
			nulls[8] = true;
			nulls[9] = true;
			nulls[10] = true;
			nulls[11] = true;
			nulls[12] = true;
			nulls[13] = true;
			nulls[14] = true;
			nulls[17] = true;
			nulls[18] = true;
			nulls[19] = true;
			nulls[20] = true;
			nulls[21] = true;
			nulls[22] = true;
			nulls[23] = true;
			nulls[24] = true;
			nulls[25] = true;
			nulls[26] = true;
			nulls[27] = true;
			nulls[28] = true;
			nulls[29] = true;
			nulls[30] = true;
		}

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

		/* 若只请求单个后端且已找到,则跳出。 */
		if (pid != -1)
			break;
	}

	return (Datum) 0;
}


Datum
pg_backend_pid(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(MyProcPid);
}


Datum
pg_stat_get_backend_pid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_INT32(beentry->st_procpid);
}


Datum
pg_stat_get_backend_dbid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_databaseid);
}


Datum
pg_stat_get_backend_userid(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	PG_RETURN_OID(beentry->st_userid);
}

Datum
pg_stat_get_backend_subxact(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBXACT_COLS	2
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_SUBXACT_COLS];
	bool		nulls[PG_STAT_GET_SUBXACT_COLS];
	int32		beid = PG_GETARG_INT32(0);
	LocalPgBackendStatus *local_beentry;

	/* 初始化 values 与 NULL 标志数组 */
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, 0, sizeof(nulls));

	/* 初始化元组描述符中的属性信息 */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_SUBXACT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "subxact_count",
					   INT4OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "subxact_overflow",
					   BOOLOID, -1, 0);

	BlessTupleDesc(tupdesc);

	if ((local_beentry = pgstat_get_local_beentry_by_backend_id(beid)) != NULL)
	{
		/* 填 values 与 NULLs */
		values[0] = Int32GetDatum(local_beentry->backend_subxact_count);
		values[1] = BoolGetDatum(local_beentry->backend_subxact_overflowed);
	}
	else
	{
		nulls[0] = true;
		nulls[1] = true;
	}

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

Datum
pg_stat_get_backend_activity(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	const char *activity;
	char	   *clipped_activity;
	text	   *ret;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		activity = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		activity = "<insufficient privilege>";
	else if (*(beentry->st_activity_raw) == '\0')
		activity = "<command string not enabled>";
	else
		activity = beentry->st_activity_raw;

	clipped_activity = pgstat_clip_activity(activity);
	ret = cstring_to_text(activity);
	pfree(clipped_activity);

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_stat_get_backend_wait_event_type(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event_type = NULL;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		wait_event_type = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event_type = "<insufficient privilege>";
	else if ((proc = BackendPidGetProc(beentry->st_procpid)) != NULL)
		wait_event_type = pgstat_get_wait_event_type(proc->wait_event_info);

	if (!wait_event_type)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event_type));
}

Datum
pg_stat_get_backend_wait_event(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	PGPROC	   *proc;
	const char *wait_event = NULL;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		wait_event = "<backend information not available>";
	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		wait_event = "<insufficient privilege>";
	else if ((proc = BackendPidGetProc(beentry->st_procpid)) != NULL)
		wait_event = pgstat_get_wait_event(proc->wait_event_info);

	if (!wait_event)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(wait_event));
}


Datum
pg_stat_get_backend_activity_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_activity_start_timestamp;

	/*
	 * 未记录当前查询开始时间 -- 用户未启用
	 * 查询级统计收集时即如此。
	 */
	if (result == 0)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_xact_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_xact_start_timestamp;

	if (result == 0)			/* 不在事务中 */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_start(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	TimestampTz result;
	PgBackendStatus *beentry;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	result = beentry->st_proc_start_timestamp;

	if (result == 0)			/* 大概不会发生? */
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_backend_client_addr(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	/* 客户端地址全零表示未知 */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(beentry->st_clientaddr.addr.ss_family, remote_host);

	PG_RETURN_DATUM(DirectFunctionCall1(inet_in,
										CStringGetDatum(remote_host)));
}

Datum
pg_stat_get_backend_client_port(PG_FUNCTION_ARGS)
{
	int32		beid = PG_GETARG_INT32(0);
	PgBackendStatus *beentry;
	SockAddr	zero_clientaddr;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if ((beentry = pgstat_get_beentry_by_backend_id(beid)) == NULL)
		PG_RETURN_NULL();

	else if (!HAS_PGSTAT_PERMISSIONS(beentry->st_userid))
		PG_RETURN_NULL();

	/* 客户端地址全零表示未知 */
	memset(&zero_clientaddr, 0, sizeof(zero_clientaddr));
	if (memcmp(&(beentry->st_clientaddr), &zero_clientaddr,
			   sizeof(zero_clientaddr)) == 0)
		PG_RETURN_NULL();

	switch (beentry->st_clientaddr.addr.ss_family)
	{
		case AF_INET:
		case AF_INET6:
			break;
		case AF_UNIX:
			PG_RETURN_INT32(-1);
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';
	ret = pg_getnameinfo_all(&beentry->st_clientaddr.addr,
							 beentry->st_clientaddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in,
										CStringGetDatum(remote_port)));
}


Datum
pg_stat_get_db_numbackends(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int32		result;
	int			tot_backends = pgstat_fetch_stat_numbackends();
	int			idx;

	result = 0;
	for (idx = 1; idx <= tot_backends; idx++)
	{
		LocalPgBackendStatus *local_beentry = pgstat_get_local_beentry_by_index(idx);

		if (local_beentry->backendStatus.st_databaseid == dbid)
			result++;
	}

	PG_RETURN_INT32(result);
}


#define PG_STAT_GET_DBENTRY_INT64(stat)							\
Datum															\
CppConcat(pg_stat_get_db_,stat)(PG_FUNCTION_ARGS)				\
{																\
	Oid			dbid = PG_GETARG_OID(0);						\
	int64		result;											\
	PgStat_StatDBEntry *dbentry;								\
																\
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)	\
		result = 0;												\
	else														\
		result = (int64) (dbentry->stat);						\
																\
	PG_RETURN_INT64(result);									\
}

/* pg_stat_get_db_blocks_fetched */
PG_STAT_GET_DBENTRY_INT64(blocks_fetched)

/* pg_stat_get_db_blocks_hit */
PG_STAT_GET_DBENTRY_INT64(blocks_hit)

/* pg_stat_get_db_conflict_bufferpin */
PG_STAT_GET_DBENTRY_INT64(conflict_bufferpin)

/* pg_stat_get_db_conflict_lock */
PG_STAT_GET_DBENTRY_INT64(conflict_lock)

/* pg_stat_get_db_conflict_snapshot */
PG_STAT_GET_DBENTRY_INT64(conflict_snapshot)

/* pg_stat_get_db_conflict_startup_deadlock */
PG_STAT_GET_DBENTRY_INT64(conflict_startup_deadlock)

/* pg_stat_get_db_conflict_tablespace */
PG_STAT_GET_DBENTRY_INT64(conflict_tablespace)

/* pg_stat_get_db_deadlocks */
PG_STAT_GET_DBENTRY_INT64(deadlocks)

/* pg_stat_get_db_sessions */
PG_STAT_GET_DBENTRY_INT64(sessions)

/* pg_stat_get_db_sessions_abandoned */
PG_STAT_GET_DBENTRY_INT64(sessions_abandoned)

/* pg_stat_get_db_sessions_fatal */
PG_STAT_GET_DBENTRY_INT64(sessions_fatal)

/* pg_stat_get_db_sessions_killed */
PG_STAT_GET_DBENTRY_INT64(sessions_killed)

/* pg_stat_get_db_temp_bytes */
PG_STAT_GET_DBENTRY_INT64(temp_bytes)

/* pg_stat_get_db_temp_files */
PG_STAT_GET_DBENTRY_INT64(temp_files)

/* pg_stat_get_db_tuples_deleted */
PG_STAT_GET_DBENTRY_INT64(tuples_deleted)

/* pg_stat_get_db_tuples_fetched */
PG_STAT_GET_DBENTRY_INT64(tuples_fetched)

/* pg_stat_get_db_tuples_inserted */
PG_STAT_GET_DBENTRY_INT64(tuples_inserted)

/* pg_stat_get_db_tuples_returned */
PG_STAT_GET_DBENTRY_INT64(tuples_returned)

/* pg_stat_get_db_tuples_updated */
PG_STAT_GET_DBENTRY_INT64(tuples_updated)

/* pg_stat_get_db_xact_commit */
PG_STAT_GET_DBENTRY_INT64(xact_commit)

/* pg_stat_get_db_xact_rollback */
PG_STAT_GET_DBENTRY_INT64(xact_rollback)

/* pg_stat_get_db_conflict_logicalslot */
PG_STAT_GET_DBENTRY_INT64(conflict_logicalslot)

Datum
pg_stat_get_db_stat_reset_time(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->stat_reset_timestamp;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}


Datum
pg_stat_get_db_conflict_all(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->conflict_tablespace +
						  dbentry->conflict_lock +
						  dbentry->conflict_snapshot +
						  dbentry->conflict_logicalslot +
						  dbentry->conflict_bufferpin +
						  dbentry->conflict_startup_deadlock);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_checksum_failures(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	int64		result;
	PgStat_StatDBEntry *dbentry;

	if (!DataChecksumsEnabled())
		PG_RETURN_NULL();

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = (int64) (dbentry->checksum_failures);

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_db_checksum_last_failure(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	TimestampTz result;
	PgStat_StatDBEntry *dbentry;

	if (!DataChecksumsEnabled())
		PG_RETURN_NULL();

	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)
		result = 0;
	else
		result = dbentry->last_checksum_failure;

	if (result == 0)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(result);
}

/* 把计数器从微秒转为毫秒以便显示 */
#define PG_STAT_GET_DBENTRY_FLOAT8_MS(stat)						\
Datum															\
CppConcat(pg_stat_get_db_,stat)(PG_FUNCTION_ARGS)				\
{																\
	Oid			dbid = PG_GETARG_OID(0);						\
	double		result;											\
	PgStat_StatDBEntry *dbentry;								\
																\
	if ((dbentry = pgstat_fetch_stat_dbentry(dbid)) == NULL)	\
		result = 0;												\
	else														\
		result = ((double) dbentry->stat) / 1000.0;				\
																\
	PG_RETURN_FLOAT8(result);									\
}

/* pg_stat_get_db_active_time */
PG_STAT_GET_DBENTRY_FLOAT8_MS(active_time)

/* pg_stat_get_db_blk_read_time */
PG_STAT_GET_DBENTRY_FLOAT8_MS(blk_read_time)

/* pg_stat_get_db_blk_write_time */
PG_STAT_GET_DBENTRY_FLOAT8_MS(blk_write_time)

/* pg_stat_get_db_idle_in_transaction_time */
PG_STAT_GET_DBENTRY_FLOAT8_MS(idle_in_transaction_time)

/* pg_stat_get_db_session_time */
PG_STAT_GET_DBENTRY_FLOAT8_MS(session_time)

Datum
pg_stat_get_bgwriter_timed_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->timed_checkpoints);
}

Datum
pg_stat_get_bgwriter_requested_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->requested_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_checkpoints(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_written_checkpoints);
}

Datum
pg_stat_get_bgwriter_buf_written_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->buf_written_clean);
}

Datum
pg_stat_get_bgwriter_maxwritten_clean(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->maxwritten_clean);
}

Datum
pg_stat_get_checkpoint_write_time(PG_FUNCTION_ARGS)
{
	/* 时间已是毫秒,仅转为 double 以便呈现 */
	PG_RETURN_FLOAT8((double)
					 pgstat_fetch_stat_checkpointer()->checkpoint_write_time);
}

Datum
pg_stat_get_checkpoint_sync_time(PG_FUNCTION_ARGS)
{
	/* 时间已是毫秒,仅转为 double 以便呈现 */
	PG_RETURN_FLOAT8((double)
					 pgstat_fetch_stat_checkpointer()->checkpoint_sync_time);
}

Datum
pg_stat_get_bgwriter_stat_reset_time(PG_FUNCTION_ARGS)
{
	PG_RETURN_TIMESTAMPTZ(pgstat_fetch_stat_bgwriter()->stat_reset_timestamp);
}

Datum
pg_stat_get_buf_written_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_written_backend);
}

Datum
pg_stat_get_buf_fsync_backend(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_checkpointer()->buf_fsync_backend);
}

Datum
pg_stat_get_buf_alloc(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(pgstat_fetch_stat_bgwriter()->buf_alloc);
}

/*
* 向 pg_stat_io 视图加新列时,在 IO_NUM_COLUMNS
* 之上新增枚举值。
*/
typedef enum io_stat_col
{
	IO_COL_INVALID = -1,
	IO_COL_BACKEND_TYPE,
	IO_COL_OBJECT,
	IO_COL_CONTEXT,
	IO_COL_READS,
	IO_COL_READ_TIME,
	IO_COL_WRITES,
	IO_COL_WRITE_TIME,
	IO_COL_WRITEBACKS,
	IO_COL_WRITEBACK_TIME,
	IO_COL_EXTENDS,
	IO_COL_EXTEND_TIME,
	IO_COL_CONVERSION,
	IO_COL_HITS,
	IO_COL_EVICTIONS,
	IO_COL_REUSES,
	IO_COL_FSYNCS,
	IO_COL_FSYNC_TIME,
	IO_COL_RESET_TIME,
	IO_NUM_COLUMNS,
} io_stat_col;

/*
 * 新增 IOOp 时,增加新的 io_stat_col,并在本函数
 * 中加 case 返回对应的 io_stat_col。
 */
static io_stat_col
pgstat_get_io_op_index(IOOp io_op)
{
	switch (io_op)
	{
		case IOOP_EVICT:
			return IO_COL_EVICTIONS;
		case IOOP_EXTEND:
			return IO_COL_EXTENDS;
		case IOOP_FSYNC:
			return IO_COL_FSYNCS;
		case IOOP_HIT:
			return IO_COL_HITS;
		case IOOP_READ:
			return IO_COL_READS;
		case IOOP_REUSE:
			return IO_COL_REUSES;
		case IOOP_WRITE:
			return IO_COL_WRITES;
		case IOOP_WRITEBACK:
			return IO_COL_WRITEBACKS;
	}

	elog(ERROR, "unrecognized IOOp value: %d", io_op);
	pg_unreachable();
}

/*
 * 取得指定 IOOp 的 IO 时间所在列号。
 * 本函数体现我们的假定:某 IOOp 的 IO 时间在视图中
 * 显示于该 IOOp 计数列的紧后一列。若操作无
 * 关联时间,则返回 IO_COL_INVALID。
 */
static io_stat_col
pgstat_get_io_time_index(IOOp io_op)
{
	switch (io_op)
	{
		case IOOP_READ:
		case IOOP_WRITE:
		case IOOP_WRITEBACK:
		case IOOP_EXTEND:
		case IOOP_FSYNC:
			return pgstat_get_io_op_index(io_op) + 1;
		case IOOP_EVICT:
		case IOOP_HIT:
		case IOOP_REUSE:
			return IO_COL_INVALID;
	}

	elog(ERROR, "unrecognized IOOp value: %d", io_op);
	pg_unreachable();
}

static inline double
pg_stat_us_to_ms(PgStat_Counter val_ms)
{
	return val_ms * (double) 0.001;
}

Datum
pg_stat_get_io(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo;
	PgStat_IO  *backends_io_stats;
	Datum		reset_time;

	InitMaterializedSRF(fcinfo, 0);
	rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	backends_io_stats = pgstat_fetch_stat_io();

	reset_time = TimestampTzGetDatum(backends_io_stats->stat_reset_timestamp);

	for (int bktype = 0; bktype < BACKEND_NUM_TYPES; bktype++)
	{
		Datum		bktype_desc = CStringGetTextDatum(GetBackendTypeDesc(bktype));
		PgStat_BktypeIO *bktype_stats = &backends_io_stats->stats[bktype];

		/*
		 * 在 Assert 构建中,可负担多一轮遍历全部
		 * 计数器,检查仅预期统计非零,因这能使
		 * 非 Assert 代码更干净。
		 */
		Assert(pgstat_bktype_io_stats_valid(bktype_stats, bktype));

		/*
		 * 对无 IO 操作统计的 BackendType,完全
		 * 不在视图中表示它们。
		 */
		if (!pgstat_tracks_io_bktype(bktype))
			continue;

		for (int io_obj = 0; io_obj < IOOBJECT_NUM_TYPES; io_obj++)
		{
			const char *obj_name = pgstat_get_io_object_name(io_obj);

			for (int io_context = 0; io_context < IOCONTEXT_NUM_TYPES; io_context++)
			{
				const char *context_name = pgstat_get_io_context_name(io_context);

				Datum		values[IO_NUM_COLUMNS] = {0};
				bool		nulls[IO_NUM_COLUMNS] = {0};

				/*
				 * 某些 BackendType、IOObject 与 IOContext
				 * 组合对任何 IOOp 都无效。此时从视图
				 * 省略整行。
				 */
				if (!pgstat_tracks_io_object(bktype, io_obj, io_context))
					continue;

				values[IO_COL_BACKEND_TYPE] = bktype_desc;
				values[IO_COL_CONTEXT] = CStringGetTextDatum(context_name);
				values[IO_COL_OBJECT] = CStringGetTextDatum(obj_name);
				values[IO_COL_RESET_TIME] = reset_time;

				/*
				 * 暂硬编码为 BLCKSZ。将来在跟踪 WAL IO
				 * 后可含 XLOG_BLCKSZ,并在跟踪非面向块
				 * 的 IO(如临时文件 IO)后可含
				 * 常数倍乘。
				 */
				values[IO_COL_CONVERSION] = Int64GetDatum(BLCKSZ);

				for (int io_op = 0; io_op < IOOP_NUM_TYPES; io_op++)
				{
					int			op_idx = pgstat_get_io_op_index(io_op);
					int			time_idx = pgstat_get_io_time_index(io_op);

					/*
					 * 某些 BackendType 与 IOOp、IOContext 与
					 * IOOp、以及 IOObject 与 IOOp 组合不跟踪。
					 * 视图中这些格置 NULL。
					 */
					if (pgstat_tracks_io_op(bktype, io_obj, io_context, io_op))
					{
						PgStat_Counter count =
							bktype_stats->counts[io_obj][io_context][io_op];

						values[op_idx] = Int64GetDatum(count);
					}
					else
						nulls[op_idx] = true;

					/* 并非每个操作都计时 */
					if (time_idx == IO_COL_INVALID)
						continue;

					if (!nulls[op_idx])
					{
						PgStat_Counter time =
							bktype_stats->times[io_obj][io_context][io_op];

						values[time_idx] = Float8GetDatum(pg_stat_us_to_ms(time));
					}
					else
						nulls[time_idx] = true;
				}

				tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
									 values, nulls);
			}
		}
	}

	return (Datum) 0;
}

/*
 * 返回 WAL 活动统计
 */
Datum
pg_stat_get_wal(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_WAL_COLS	9
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_WAL_COLS] = {0};
	bool		nulls[PG_STAT_GET_WAL_COLS] = {0};
	char		buf[256];
	PgStat_WalStats *wal_stats;

	/* 初始化元组描述符中的属性信息 */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_WAL_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "wal_records",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "wal_fpi",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "wal_bytes",
					   NUMERICOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "wal_buffers_full",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "wal_write",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "wal_sync",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "wal_write_time",
					   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "wal_sync_time",
					   FLOAT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);

	BlessTupleDesc(tupdesc);

	/* 获取 WAL 活动统计 */
	wal_stats = pgstat_fetch_stat_wal();

	/* 填 values 与 NULLs */
	values[0] = Int64GetDatum(wal_stats->wal_records);
	values[1] = Int64GetDatum(wal_stats->wal_fpi);

	/* 转为 numeric。 */
	snprintf(buf, sizeof buf, UINT64_FORMAT, wal_stats->wal_bytes);
	values[2] = DirectFunctionCall3(numeric_in,
									CStringGetDatum(buf),
									ObjectIdGetDatum(0),
									Int32GetDatum(-1));

	values[3] = Int64GetDatum(wal_stats->wal_buffers_full);
	values[4] = Int64GetDatum(wal_stats->wal_write);
	values[5] = Int64GetDatum(wal_stats->wal_sync);

	/* 把计数器从微秒转为毫秒以便显示 */
	values[6] = Float8GetDatum(((double) wal_stats->wal_write_time) / 1000.0);
	values[7] = Float8GetDatum(((double) wal_stats->wal_sync_time) / 1000.0);

	values[8] = TimestampTzGetDatum(wal_stats->stat_reset_timestamp);

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * 返回 SLRU 缓存统计。
 */
Datum
pg_stat_get_slru(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SLRU_COLS	9
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			i;
	PgStat_SLRUStats *stats;

	InitMaterializedSRF(fcinfo, 0);

	/* 从累计统计系统请求 SLRU 统计 */
	stats = pgstat_fetch_slru();

	for (i = 0;; i++)
	{
		/* 每行 */
		Datum		values[PG_STAT_GET_SLRU_COLS] = {0};
		bool		nulls[PG_STAT_GET_SLRU_COLS] = {0};
		PgStat_SLRUStats stat;
		const char *name;

		name = pgstat_get_slru_name(i);

		if (!name)
			break;

		stat = stats[i];

		values[0] = PointerGetDatum(cstring_to_text(name));
		values[1] = Int64GetDatum(stat.blocks_zeroed);
		values[2] = Int64GetDatum(stat.blocks_hit);
		values[3] = Int64GetDatum(stat.blocks_read);
		values[4] = Int64GetDatum(stat.blocks_written);
		values[5] = Int64GetDatum(stat.blocks_exists);
		values[6] = Int64GetDatum(stat.flush);
		values[7] = Int64GetDatum(stat.truncate);
		values[8] = TimestampTzGetDatum(stat.stat_reset_timestamp);

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}

#define PG_STAT_GET_XACT_RELENTRY_INT64(stat)			\
Datum													\
CppConcat(pg_stat_get_xact_,stat)(PG_FUNCTION_ARGS)		\
{														\
	Oid         relid = PG_GETARG_OID(0);				\
	int64       result;									\
	PgStat_TableStatus *tabentry;						\
														\
	if ((tabentry = find_tabstat_entry(relid)) == NULL)	\
		result = 0;										\
	else												\
		result = (int64) (tabentry->counts.stat);		\
														\
	PG_RETURN_INT64(result);							\
}

/* pg_stat_get_xact_numscans */
PG_STAT_GET_XACT_RELENTRY_INT64(numscans)

/* pg_stat_get_xact_tuples_returned */
PG_STAT_GET_XACT_RELENTRY_INT64(tuples_returned)

/* pg_stat_get_xact_tuples_fetched */
PG_STAT_GET_XACT_RELENTRY_INT64(tuples_fetched)

/* pg_stat_get_xact_tuples_hot_updated */
PG_STAT_GET_XACT_RELENTRY_INT64(tuples_hot_updated)

/* pg_stat_get_xact_tuples_newpage_updated */
PG_STAT_GET_XACT_RELENTRY_INT64(tuples_newpage_updated)

/* pg_stat_get_xact_blocks_fetched */
PG_STAT_GET_XACT_RELENTRY_INT64(blocks_fetched)

/* pg_stat_get_xact_blocks_hit */
PG_STAT_GET_XACT_RELENTRY_INT64(blocks_hit)

Datum
pg_stat_get_xact_tuples_inserted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->counts.tuples_inserted;
		/* 活动子事务计数尚未计入 tuples_inserted */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_inserted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_updated(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->counts.tuples_updated;
		/* 活动子事务计数尚未计入 tuples_updated */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_updated;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_tuples_deleted(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	int64		result;
	PgStat_TableStatus *tabentry;
	PgStat_TableXactStatus *trans;

	if ((tabentry = find_tabstat_entry(relid)) == NULL)
		result = 0;
	else
	{
		result = tabentry->counts.tuples_deleted;
		/* 活动子事务计数尚未计入 tuples_deleted */
		for (trans = tabentry->trans; trans != NULL; trans = trans->upper)
			result += trans->tuples_deleted;
	}

	PG_RETURN_INT64(result);
}

Datum
pg_stat_get_xact_function_calls(PG_FUNCTION_ARGS)
{
	Oid			funcid = PG_GETARG_OID(0);
	PgStat_FunctionCounts *funcentry;

	if ((funcentry = find_funcstat_entry(funcid)) == NULL)
		PG_RETURN_NULL();
	PG_RETURN_INT64(funcentry->numcalls);
}

#define PG_STAT_GET_XACT_FUNCENTRY_FLOAT8_MS(stat)				\
Datum															\
CppConcat(pg_stat_get_xact_function_,stat)(PG_FUNCTION_ARGS)	\
{																\
	Oid			funcid = PG_GETARG_OID(0);						\
	PgStat_FunctionCounts *funcentry;							\
																\
	if ((funcentry = find_funcstat_entry(funcid)) == NULL)		\
		PG_RETURN_NULL();										\
	PG_RETURN_FLOAT8(INSTR_TIME_GET_MILLISEC(funcentry->stat));	\
}

/* pg_stat_get_xact_function_total_time */
PG_STAT_GET_XACT_FUNCENTRY_FLOAT8_MS(total_time)

/* pg_stat_get_xact_function_self_time */
PG_STAT_GET_XACT_FUNCENTRY_FLOAT8_MS(self_time)

/* 获取当前统计快照的时间戳 */
Datum
pg_stat_get_snapshot_timestamp(PG_FUNCTION_ARGS)
{
	bool		have_snapshot;
	TimestampTz ts;

	ts = pgstat_get_stat_snapshot_timestamp(&have_snapshot);

	if (!have_snapshot)
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(ts);
}

/* 丢弃活动的统计快照 */
Datum
pg_stat_clear_snapshot(PG_FUNCTION_ARGS)
{
	pgstat_clear_snapshot();

	PG_RETURN_VOID();
}


/* 强制在下次时机上报统计 */
Datum
pg_stat_force_next_flush(PG_FUNCTION_ARGS)
{
	pgstat_force_next_flush();

	PG_RETURN_VOID();
}


/* 重置当前数据库的全部计数器 */
Datum
pg_stat_reset(PG_FUNCTION_ARGS)
{
	pgstat_reset_counters();

	PG_RETURN_VOID();
}

/*
 * 重置若干共享集簇级计数器
 *
 * 新增重置目标时,名称最好与 pgstat_kind_infos
 * 中的一致(若相关)。
 */
Datum
pg_stat_reset_shared(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (strcmp(target, "archiver") == 0)
		pgstat_reset_of_kind(PGSTAT_KIND_ARCHIVER);
	else if (strcmp(target, "bgwriter") == 0)
	{
		/*
		 * 历史上 checkpointer 属 bgwriter,现仍一并
		 * 重置。
		 */
		pgstat_reset_of_kind(PGSTAT_KIND_BGWRITER);
		pgstat_reset_of_kind(PGSTAT_KIND_CHECKPOINTER);
	}
	else if (strcmp(target, "io") == 0)
		pgstat_reset_of_kind(PGSTAT_KIND_IO);
	else if (strcmp(target, "recovery_prefetch") == 0)
		XLogPrefetchResetStats();
	else if (strcmp(target, "wal") == 0)
		pgstat_reset_of_kind(PGSTAT_KIND_WAL);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unrecognized reset target: \"%s\"", target),
				 errhint("Target must be \"archiver\", \"bgwriter\", \"io\", \"recovery_prefetch\", or \"wal\".")));

	PG_RETURN_VOID();
}

/*
 * 重置单个对象的统计,该对象可属当前
 * 数据库,或在集簇内各库间共享。
 */
Datum
pg_stat_reset_single_table_counters(PG_FUNCTION_ARGS)
{
	Oid			taboid = PG_GETARG_OID(0);
	Oid			dboid = (IsSharedRelation(taboid) ? InvalidOid : MyDatabaseId);

	pgstat_reset(PGSTAT_KIND_RELATION, dboid, taboid);

	PG_RETURN_VOID();
}

Datum
pg_stat_reset_single_function_counters(PG_FUNCTION_ARGS)
{
	Oid			funcoid = PG_GETARG_OID(0);

	pgstat_reset(PGSTAT_KIND_FUNCTION, MyDatabaseId, funcoid);

	PG_RETURN_VOID();
}

/* 重置 SLRU 计数器(某个或全部)。 */
Datum
pg_stat_reset_slru(PG_FUNCTION_ARGS)
{
	char	   *target = NULL;

	if (PG_ARGISNULL(0))
		pgstat_reset_of_kind(PGSTAT_KIND_SLRU);
	else
	{
		target = text_to_cstring(PG_GETARG_TEXT_PP(0));
		pgstat_reset_slru(target);
	}

	PG_RETURN_VOID();
}

/* 重置复制槽统计(某个或全部)。 */
Datum
pg_stat_reset_replication_slot(PG_FUNCTION_ARGS)
{
	char	   *target = NULL;

	if (PG_ARGISNULL(0))
		pgstat_reset_of_kind(PGSTAT_KIND_REPLSLOT);
	else
	{
		target = text_to_cstring(PG_GETARG_TEXT_PP(0));
		pgstat_reset_replslot(target);
	}

	PG_RETURN_VOID();
}

/* 重置订阅统计(某个或全部) */
Datum
pg_stat_reset_subscription_stats(PG_FUNCTION_ARGS)
{
	Oid			subid;

	if (PG_ARGISNULL(0))
	{
		/* 清除全部订阅统计 */
		pgstat_reset_of_kind(PGSTAT_KIND_SUBSCRIPTION);
	}
	else
	{
		subid = PG_GETARG_OID(0);

		if (!OidIsValid(subid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid subscription OID %u", subid)));
		pgstat_reset(PGSTAT_KIND_SUBSCRIPTION, InvalidOid, subid);
	}

	PG_RETURN_VOID();
}

Datum
pg_stat_get_archiver(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[7] = {0};
	bool		nulls[7] = {0};
	PgStat_ArchiverStats *archiver_stats;

	/* 初始化元组描述符中的属性信息 */
	tupdesc = CreateTemplateTupleDesc(7);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "archived_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "last_archived_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "last_archived_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "failed_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "last_failed_wal",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "last_failed_time",
					   TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);

	BlessTupleDesc(tupdesc);

	/* 获取归档进程统计 */
	archiver_stats = pgstat_fetch_stat_archiver();

	/* 填 values 与 NULLs */
	values[0] = Int64GetDatum(archiver_stats->archived_count);
	if (*(archiver_stats->last_archived_wal) == '\0')
		nulls[1] = true;
	else
		values[1] = CStringGetTextDatum(archiver_stats->last_archived_wal);

	if (archiver_stats->last_archived_timestamp == 0)
		nulls[2] = true;
	else
		values[2] = TimestampTzGetDatum(archiver_stats->last_archived_timestamp);

	values[3] = Int64GetDatum(archiver_stats->failed_count);
	if (*(archiver_stats->last_failed_wal) == '\0')
		nulls[4] = true;
	else
		values[4] = CStringGetTextDatum(archiver_stats->last_failed_wal);

	if (archiver_stats->last_failed_timestamp == 0)
		nulls[5] = true;
	else
		values[5] = TimestampTzGetDatum(archiver_stats->last_failed_timestamp);

	if (archiver_stats->stat_reset_timestamp == 0)
		nulls[6] = true;
	else
		values[6] = TimestampTzGetDatum(archiver_stats->stat_reset_timestamp);

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * 获取复制槽统计。若槽统计不可用,
 * 则返回全零统计。
 */
Datum
pg_stat_get_replication_slot(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_REPLICATION_SLOT_COLS 10
	text	   *slotname_text = PG_GETARG_TEXT_P(0);
	NameData	slotname;
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_REPLICATION_SLOT_COLS] = {0};
	bool		nulls[PG_STAT_GET_REPLICATION_SLOT_COLS] = {0};
	PgStat_StatReplSlotEntry *slotent;
	PgStat_StatReplSlotEntry allzero;

	/* 初始化元组描述符中的属性信息 */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_REPLICATION_SLOT_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "slot_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "spill_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "spill_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "spill_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "stream_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "stream_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 7, "stream_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 8, "total_txns",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 9, "total_bytes",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 10, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);
	BlessTupleDesc(tupdesc);

	namestrcpy(&slotname, text_to_cstring(slotname_text));
	slotent = pgstat_fetch_replslot(slotname);
	if (!slotent)
	{
		/*
		 * 若未找到槽则初始化其统计。创建槽
		 * 消息丢失时可能如此。
		 */
		memset(&allzero, 0, sizeof(PgStat_StatReplSlotEntry));
		slotent = &allzero;
	}

	values[0] = CStringGetTextDatum(NameStr(slotname));
	values[1] = Int64GetDatum(slotent->spill_txns);
	values[2] = Int64GetDatum(slotent->spill_count);
	values[3] = Int64GetDatum(slotent->spill_bytes);
	values[4] = Int64GetDatum(slotent->stream_txns);
	values[5] = Int64GetDatum(slotent->stream_count);
	values[6] = Int64GetDatum(slotent->stream_bytes);
	values[7] = Int64GetDatum(slotent->total_txns);
	values[8] = Int64GetDatum(slotent->total_bytes);

	if (slotent->stat_reset_timestamp == 0)
		nulls[9] = true;
	else
		values[9] = TimestampTzGetDatum(slotent->stat_reset_timestamp);

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * 获取给定订阅的订阅统计。若
 * 订阅统计不可用,则返回全零统计。
 */
Datum
pg_stat_get_subscription_stats(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_SUBSCRIPTION_STATS_COLS	4
	Oid			subid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[PG_STAT_GET_SUBSCRIPTION_STATS_COLS] = {0};
	bool		nulls[PG_STAT_GET_SUBSCRIPTION_STATS_COLS] = {0};
	PgStat_StatSubEntry *subentry;
	PgStat_StatSubEntry allzero;

	/* 获取订阅统计 */
	subentry = pgstat_fetch_stat_subscription(subid);

	/* 初始化元组描述符中的属性信息 */
	tupdesc = CreateTemplateTupleDesc(PG_STAT_GET_SUBSCRIPTION_STATS_COLS);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "subid",
					   OIDOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "apply_error_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "sync_error_count",
					   INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "stats_reset",
					   TIMESTAMPTZOID, -1, 0);
	BlessTupleDesc(tupdesc);

	if (!subentry)
	{
		/* 若未找到订阅则初始化其统计 */
		memset(&allzero, 0, sizeof(PgStat_StatSubEntry));
		subentry = &allzero;
	}

	/* subid */
	values[0] = ObjectIdGetDatum(subid);

	/* apply_error_count */
	values[1] = Int64GetDatum(subentry->apply_error_count);

	/* sync_error_count */
	values[2] = Int64GetDatum(subentry->sync_error_count);

	/* stats_reset */
	if (subentry->stat_reset_timestamp == 0)
		nulls[3] = true;
	else
		values[3] = TimestampTzGetDatum(subentry->stat_reset_timestamp);

	/* 以 Datum 返回该记录 */
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * 检查给定 kind、库 oid、对象 oid 是否有统计。
 * 
 *
 * 这对测试有用,别无他用,故不建档。
 * 
 */
Datum
pg_stat_have_stats(PG_FUNCTION_ARGS)
{
	char	   *stats_type = text_to_cstring(PG_GETARG_TEXT_P(0));
	Oid			dboid = PG_GETARG_OID(1);
	Oid			objoid = PG_GETARG_OID(2);
	PgStat_Kind kind = pgstat_get_kind_from_str(stats_type);

	PG_RETURN_BOOL(pgstat_have_entry(kind, dboid, objoid));
}
