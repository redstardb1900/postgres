/*-------------------------------------------------------------------------
 *
 * pqmq.c
 *	  用前端/后端协议经 shm_mq 进行通信
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	src/backend/libpq/pqmq.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/parallel.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "libpq/pqmq.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/logicalworker.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"

static shm_mq_handle *pq_mq_handle;
static bool pq_mq_busy = false;
static pid_t pq_mq_parallel_leader_pid = 0;
static pid_t pq_mq_parallel_leader_backend_id = InvalidBackendId;

static void pq_cleanup_redirect_to_shm_mq(dsm_segment *seg, Datum arg);
static void mq_comm_reset(void);
static int	mq_flush(void);
static int	mq_flush_if_writable(void);
static bool mq_is_send_pending(void);
static int	mq_putmessage(char msgtype, const char *s, size_t len);
static void mq_putmessage_noblock(char msgtype, const char *s, size_t len);

static const PQcommMethods PqCommMqMethods = {
	mq_comm_reset,
	mq_flush,
	mq_flush_if_writable,
	mq_is_send_pending,
	mq_putmessage,
	mq_putmessage_noblock
};

/*
 * 安排把前端/后端协议消息重定向到共享内存消息队列。
 *
 */
void
pq_redirect_to_shm_mq(dsm_segment *seg, shm_mq_handle *mqh)
{
	PqCommMethods = &PqCommMqMethods;
	pq_mq_handle = mqh;
	whereToSendOutput = DestRemote;
	FrontendProtocol = PG_PROTOCOL_LATEST;
	on_dsm_detach(seg, pq_cleanup_redirect_to_shm_mq, (Datum) 0);
}

/*
 * 当包含我们 shm_mq 的 DSM 消失时，需要停止向它发送消息。
 *
 */
static void
pq_cleanup_redirect_to_shm_mq(dsm_segment *seg, Datum arg)
{
	pq_mq_handle = NULL;
	whereToSendOutput = DestNone;
}

/*
 * 安排每次通过 shm_mq 传送消息数据时，向并行领导者
 * SendProcSignal()。
 */
void
pq_set_parallel_leader(pid_t pid, BackendId backend_id)
{
	Assert(PqCommMethods == &PqCommMqMethods);
	pq_mq_parallel_leader_pid = pid;
	pq_mq_parallel_leader_backend_id = backend_id;
}

static void
mq_comm_reset(void)
{
	/* 无事可做。 */
}

static int
mq_flush(void)
{
	/* 无事可做。 */
	return 0;
}

static int
mq_flush_if_writable(void)
{
	/* 无事可做。 */
	return 0;
}

static bool
mq_is_send_pending(void)
{
	/* 从不会有任何待处理内容。 */
	return 0;
}

/*
 * 经 pq_mq_handle 选定的共享内存消息队列传送一条 libpq
 * 协议消息。我们不附带长度字，因为接收方会从
 * shm_mq_receive() 知道消息长度。
 */
static int
mq_putmessage(char msgtype, const char *s, size_t len)
{
	shm_mq_iovec iov[2];
	shm_mq_result result;

	/*
 * 若我们正在发送消息，因为队列已满而必须等待，然后被中断，
 * 而该中断又导致试图再发送一条消息，我们就通过分离队列来
 * 回应。没有办法回到原来的上下文，即便有，仅仅把消息入队
 * 也等于无限期推迟对该中断的响应。因此我们改这样做。
 *
 *
 */
	if (pq_mq_busy)
	{
		if (pq_mq_handle != NULL)
			shm_mq_detach(pq_mq_handle);
		pq_mq_handle = NULL;
		return EOF;
	}

	/*
 * 若消息队列已经消失，就忽略这条消息。这不一定表示有问题；
 * 例如，DEBUG 消息可能在关闭序列的后期产生，那时所有 DSMs
 * 都已经分离。
 *
 */
	if (pq_mq_handle == NULL)
		return 0;

	pq_mq_busy = true;

	iov[0].data = &msgtype;
	iov[0].len = 1;
	iov[1].data = s;
	iov[1].len = len;

	Assert(pq_mq_handle != NULL);

	for (;;)
	{
		/*
 * 通过把 force_flush 传为 true 立即通知接收方，这样在紧接着
 * 发送并行消息信号之前，共享内存中的值已经更新。
 *
 */
		result = shm_mq_sendv(pq_mq_handle, iov, 2, true, true);

		if (pq_mq_parallel_leader_pid != 0)
		{
			if (IsLogicalParallelApplyWorker())
				SendProcSignal(pq_mq_parallel_leader_pid,
							   PROCSIG_PARALLEL_APPLY_MESSAGE,
							   pq_mq_parallel_leader_backend_id);
			else
			{
				Assert(IsParallelWorker());
				SendProcSignal(pq_mq_parallel_leader_pid,
							   PROCSIG_PARALLEL_MESSAGE,
							   pq_mq_parallel_leader_backend_id);
			}
		}

		if (result != SHM_MQ_WOULD_BLOCK)
			break;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0,
						 WAIT_EVENT_MQ_PUT_MESSAGE);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}

	pq_mq_busy = false;

	Assert(result == SHM_MQ_SUCCESS || result == SHM_MQ_DETACHED);
	if (result != SHM_MQ_SUCCESS)
		return EOF;
	return 0;
}

static void
mq_putmessage_noblock(char msgtype, const char *s, size_t len)
{
	/*
 * shm_mq 机制确实支持以非阻塞模式发送消息，但目前没有办法
 * 尝试开始发送消息、而又不让我们承诺完成这次传输。将来
 * 可以改进，但现在我们不需要。
 *
 *
 */
	elog(ERROR, "not currently supported");
}

/*
 * 解析 ErrorResponse 或 NoticeResponse 的负载，并用结果填充
 * 一个 ErrorData 结构。
 */
void
pq_parse_errornotice(StringInfo msg, ErrorData *edata)
{
	/* 用合理的默认值初始化 edata。 */
	MemSet(edata, 0, sizeof(ErrorData));
	edata->elevel = ERROR;
	edata->assoc_context = CurrentMemoryContext;

	/* 遍历各字段并逐个取出。 */
	for (;;)
	{
		char		code = pq_getmsgbyte(msg);
		const char *value;

		if (code == '\0')
		{
			pq_getmsgend(msg);
			break;
		}
		value = pq_getmsgrawstring(msg);

		switch (code)
		{
			case PG_DIAG_SEVERITY:
				/* 忽略，相信我们会得到未本地化的版本 */
				break;
			case PG_DIAG_SEVERITY_NONLOCALIZED:
				if (strcmp(value, "DEBUG") == 0)
				{
					/*
 * 我们无法重建确切的 DEBUG 级别，但推测它 >= client_min_messages，
 * 因此选用 DEBUG1，以确保会把它传给客户端。
 *
 */
					edata->elevel = DEBUG1;
				}
				else if (strcmp(value, "LOG") == 0)
				{
					/*
 * 它不会是 LOG_SERVER_ONLY，否则工作进程不会把它发给我们；
 * 因此 LOG 是正确的值。
 */
					edata->elevel = LOG;
				}
				else if (strcmp(value, "INFO") == 0)
					edata->elevel = INFO;
				else if (strcmp(value, "NOTICE") == 0)
					edata->elevel = NOTICE;
				else if (strcmp(value, "WARNING") == 0)
					edata->elevel = WARNING;
				else if (strcmp(value, "ERROR") == 0)
					edata->elevel = ERROR;
				else if (strcmp(value, "FATAL") == 0)
					edata->elevel = FATAL;
				else if (strcmp(value, "PANIC") == 0)
					edata->elevel = PANIC;
				else
					elog(ERROR, "unrecognized error severity: \"%s\"", value);
				break;
			case PG_DIAG_SQLSTATE:
				if (strlen(value) != 5)
					elog(ERROR, "invalid SQLSTATE: \"%s\"", value);
				edata->sqlerrcode = MAKE_SQLSTATE(value[0], value[1], value[2],
												  value[3], value[4]);
				break;
			case PG_DIAG_MESSAGE_PRIMARY:
				edata->message = pstrdup(value);
				break;
			case PG_DIAG_MESSAGE_DETAIL:
				edata->detail = pstrdup(value);
				break;
			case PG_DIAG_MESSAGE_HINT:
				edata->hint = pstrdup(value);
				break;
			case PG_DIAG_STATEMENT_POSITION:
				edata->cursorpos = pg_strtoint32(value);
				break;
			case PG_DIAG_INTERNAL_POSITION:
				edata->internalpos = pg_strtoint32(value);
				break;
			case PG_DIAG_INTERNAL_QUERY:
				edata->internalquery = pstrdup(value);
				break;
			case PG_DIAG_CONTEXT:
				edata->context = pstrdup(value);
				break;
			case PG_DIAG_SCHEMA_NAME:
				edata->schema_name = pstrdup(value);
				break;
			case PG_DIAG_TABLE_NAME:
				edata->table_name = pstrdup(value);
				break;
			case PG_DIAG_COLUMN_NAME:
				edata->column_name = pstrdup(value);
				break;
			case PG_DIAG_DATATYPE_NAME:
				edata->datatype_name = pstrdup(value);
				break;
			case PG_DIAG_CONSTRAINT_NAME:
				edata->constraint_name = pstrdup(value);
				break;
			case PG_DIAG_SOURCE_FILE:
				edata->filename = pstrdup(value);
				break;
			case PG_DIAG_SOURCE_LINE:
				edata->lineno = pg_strtoint32(value);
				break;
			case PG_DIAG_SOURCE_FUNCTION:
				edata->funcname = pstrdup(value);
				break;
			default:
				elog(ERROR, "unrecognized error field code: %d", (int) code);
				break;
		}
	}
}
