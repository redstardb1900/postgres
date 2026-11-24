/*-------------------------------------------------------------------------
 *
 * basebackup_copy.c
 *	  用 COPY OUT 发送基备份归档
 *
 * 在开始 COPY OUT 前，先发送含待纳入备份之表空间信息的
 * 结果集。然后启动单次 COPY OUT，在该次过程中传送
 * 全部归档以及（若有）清单。每条 CopyData 消息以类型
 * 字节开头，从而可用非结束 COPY 流的方式标示新归档
 * 或清单开始。这也便于未来协议扩展：只要确信
 * 客户端知道如何处理，就可在消息流中放入任意信息。
 *
 *
 *
 *
 * 旧法为每个归档单独一次 COPY OUT，现已
 * 不再支持。
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "backup/basebackup.h"
#include "backup/basebackup_sink.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "tcop/dest.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

typedef struct bbsink_copystream
{
	/* 各类 sink 的公共信息。 */
	bbsink		base;

	/* 归档是发给客户端，还是发往别处？ */
	bool		send_to_client;

	/*
	 * 协议消息缓冲。组装 CopyData 协议消息时，把本缓
	 * 冲首字节设为 'd'（归档或清单数据），再让
	 * base.bbs_buffer 指向第二字节，使其余数据拷到
	 * 消息中我们想要的位置。
	 *
	 */
	char	   *msgbuffer;

	/*
	 * 上次向客户端报告进度是何时，以及报告了多少
	 * 进度？
	 */
	TimestampTz last_progress_report_time;
	uint64		bytes_done_at_last_time_check;
} bbsink_copystream;

/*
 * 不想过于频繁地向客户端发进度消息。理想是距
 * 上次达 PROGRESS_REPORT_MILLISECOND_THRESHOLD 再发，
 * 但每次发送一点点数据都查系统时间太贵。故仅当
 * 自上次检查以来字节数达到
 * PROGRESS_REPORT_BYTE_INTERVAL
 * 后才检查时间。
 */
#define	PROGRESS_REPORT_BYTE_INTERVAL				65536
#define PROGRESS_REPORT_MILLISECOND_THRESHOLD		1000

static void bbsink_copystream_begin_backup(bbsink *sink);
static void bbsink_copystream_begin_archive(bbsink *sink,
											const char *archive_name);
static void bbsink_copystream_archive_contents(bbsink *sink, size_t len);
static void bbsink_copystream_end_archive(bbsink *sink);
static void bbsink_copystream_begin_manifest(bbsink *sink);
static void bbsink_copystream_manifest_contents(bbsink *sink, size_t len);
static void bbsink_copystream_end_manifest(bbsink *sink);
static void bbsink_copystream_end_backup(bbsink *sink, XLogRecPtr endptr,
										 TimeLineID endtli);
static void bbsink_copystream_cleanup(bbsink *sink);

static void SendCopyOutResponse(void);
static void SendCopyDone(void);
static void SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli);
static void SendTablespaceList(List *tablespaces);

static const bbsink_ops bbsink_copystream_ops = {
	.begin_backup = bbsink_copystream_begin_backup,
	.begin_archive = bbsink_copystream_begin_archive,
	.archive_contents = bbsink_copystream_archive_contents,
	.end_archive = bbsink_copystream_end_archive,
	.begin_manifest = bbsink_copystream_begin_manifest,
	.manifest_contents = bbsink_copystream_manifest_contents,
	.end_manifest = bbsink_copystream_end_manifest,
	.end_backup = bbsink_copystream_end_backup,
	.cleanup = bbsink_copystream_cleanup
};

/*
 * 新建 'copystream' bbsink。
 */
bbsink *
bbsink_copystream_new(bool send_to_client)
{
	bbsink_copystream *sink = palloc0(sizeof(bbsink_copystream));

	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_copystream_ops;
	sink->send_to_client = send_to_client;

	/* 设置周期性进度报告。 */
	sink->last_progress_report_time = GetCurrentTimestamp();
	sink->bytes_done_at_last_time_check = UINT64CONST(0);

	return &sink->base;
}

/*
 * 发送备份开始有线协议消息。
 */
static void
bbsink_copystream_begin_backup(bbsink *sink)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = sink->bbs_state;
	char	   *buf;

	/*
	 * 初始化缓冲。最终以 CopyData 消息发送归档与清单
	 * 数据，且每条载荷以类型字节开头。但 basebackup.c
	 * 预期缓冲对齐，故不能只为类型字节多分配1字节
	 * 改为多分配足够字节，使暴露给调用方部分可对齐，
	 * 同时在其前留下塞入类型字节的空间。这样就能用
	 * 单次 pq_putmessage 发出且无需额外拷贝。
	 *
	 *
	 *
	 */
	buf = palloc(mysink->base.bbs_buffer_length + MAXIMUM_ALIGNOF);
	mysink->msgbuffer = buf + (MAXIMUM_ALIGNOF - 1);
	mysink->base.bbs_buffer = buf + MAXIMUM_ALIGNOF;
	mysink->msgbuffer[0] = 'd'; /* 归档或清单数据 */

	/* 告知客户端备份起始位置。 */
	SendXlogRecPtrResult(state->startptr, state->starttli);

	/* 向客户端发送表空间列表. */
	SendTablespaceList(state->tablespaces);

	/* 发送 CommandComplete 消息 */
	pq_puttextmessage('C', "SELECT");

	/* 开始 COPY 流。将用于全部归档+清单。 */
	SendCopyOutResponse();
}

/*
 * 发送宣布新归档开始的 CopyData 消息。
 */
static void
bbsink_copystream_begin_archive(bbsink *sink, const char *archive_name)
{
	bbsink_state *state = sink->bbs_state;
	tablespaceinfo *ti;
	StringInfoData buf;

	ti = list_nth(state->tablespaces, state->tablespace_num);
	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'n');		/* 新归档 */
	pq_sendstring(&buf, archive_name);
	pq_sendstring(&buf, ti->path == NULL ? "" : ti->path);
	pq_endmessage(&buf);
}

/*
 * 发送含一块归档内容的 CopyData 消息。
 */
static void
bbsink_copystream_archive_contents(bbsink *sink, size_t len)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = mysink->base.bbs_state;
	StringInfoData buf;
	uint64		targetbytes;

	/* 若合适，把归档内容发给客户端。 */
	if (mysink->send_to_client)
	{
		/* 加一，因还要发送前导类型字节。 */
		pq_putmessage('d', mysink->msgbuffer, len + 1);
	}

	/* 考虑是否向客户端发送进度报告。 */
	targetbytes = mysink->bytes_done_at_last_time_check
		+ PROGRESS_REPORT_BYTE_INTERVAL;
	if (targetbytes <= state->bytes_done)
	{
		TimestampTz now = GetCurrentTimestamp();
		long		ms;

		/*
		 * 已发送相当字节数，故检查系统时间看是否该发
		 * 进度报告。
		 */
		mysink->bytes_done_at_last_time_check = state->bytes_done;
		ms = TimestampDifferenceMilliseconds(mysink->last_progress_report_time,
											 now);

		/*
		 * 若已过足够时间则发进度报告。若系统时钟被回拨
		 * 也发一份，以免此类情况压制后续进度消息。
		 * 压制后续消息。
		 */
		if (ms >= PROGRESS_REPORT_MILLISECOND_THRESHOLD ||
			now < mysink->last_progress_report_time)
		{
			mysink->last_progress_report_time = now;

			pq_beginmessage(&buf, 'd'); /* CopyData */
			pq_sendbyte(&buf, 'p'); /* 进度报告 */
			pq_sendint64(&buf, state->bytes_done);
			pq_endmessage(&buf);
			pq_flush_if_writable();
		}
	}
}

/*
 * 不必显式标示归档结束；客户端会在我们开始下一
 * 归档、开始清单或结束 COPY 流时判明已到末尾。
 * 但此时强制发一份进度报告似乎合适。原因之一是
 * 若这是最后一个归档且现在不强制报告，客户端将
 * 永远不知我们已发完全部字节。
 * 永远不知已发完。
 */
static void
bbsink_copystream_end_archive(bbsink *sink)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;
	bbsink_state *state = mysink->base.bbs_state;
	StringInfoData buf;

	mysink->bytes_done_at_last_time_check = state->bytes_done;
	mysink->last_progress_report_time = GetCurrentTimestamp();
	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'p');		/* 进度报告 */
	pq_sendint64(&buf, state->bytes_done);
	pq_endmessage(&buf);
	pq_flush_if_writable();
}

/*
 * 发送宣布备份清单开始的 CopyData 消息。
 */
static void
bbsink_copystream_begin_manifest(bbsink *sink)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'd'); /* CopyData */
	pq_sendbyte(&buf, 'm');		/* 清单 */
	pq_endmessage(&buf);
}

/*
 * 清单数据的每一块用 CopyData 消息发送。
 */
static void
bbsink_copystream_manifest_contents(bbsink *sink, size_t len)
{
	bbsink_copystream *mysink = (bbsink_copystream *) sink;

	if (mysink->send_to_client)
	{
		/* 加一，因还要发送前导类型字节。 */
		pq_putmessage('d', mysink->msgbuffer, len + 1);
	}
}

/*
 * 备份清单不需要显式终止符。
 */
static void
bbsink_copystream_end_manifest(bbsink *sink)
{
	/* 无操作. */
}

/*
 * 发送备份结束有线协议消息。
 */
static void
bbsink_copystream_end_backup(bbsink *sink, XLogRecPtr endptr,
							 TimeLineID endtli)
{
	SendCopyDone();
	SendXlogRecPtrResult(endptr, endtli);
}

/*
 * 清理.
 */
static void
bbsink_copystream_cleanup(bbsink *sink)
{
	/* 无事可做. */
}

/*
 * 发送 CopyOutResponse.
 */
static void
SendCopyOutResponse(void)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* 总体格式 */
	pq_sendint16(&buf, 0);		/* natts */
	pq_endmessage(&buf);
}

/*
 * 发送 CopyDone 消息.
 */
static void
SendCopyDone(void)
{
	pq_putemptymessage('c');
}

/*
 * 发送仅含单条记录的结果集：一条
 * XLogRecPtr（文本格式）
 */
static void
SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli)
{
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[2];
	bool		nulls[2] = {0};

	dest = CreateDestReceiver(DestRemoteSimple);

	tupdesc = CreateTemplateTupleDesc(2);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "recptr", TEXTOID, -1, 0);

	/*
	 * 对此用 int8 看似奇怪，但理论上 int4 宽度不够，
	 * 因 TimeLineID 是无符号的。
	 */
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "tli", INT8OID, -1, 0);

	/* 发 RowDescription */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* 数据. */
	values[0] = CStringGetTextDatum(psprintf("%X/%X", LSN_FORMAT_ARGS(ptr)));
	values[1] = Int64GetDatum(tli);
	do_tup_output(tstate, values, nulls);

	end_tup_output(tstate);

	/* 发送 CommandComplete 消息 */
	pq_puttextmessage('C', "SELECT");
}

/*
 * 经 libpq 发送描述表空间列表的结果集。
 */
static void
SendTablespaceList(List *tablespaces)
{
	DestReceiver *dest;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	ListCell   *lc;

	dest = CreateDestReceiver(DestRemoteSimple);

	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "spcoid", OIDOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "spclocation", TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "size", INT8OID, -1, 0);

	/* 发 RowDescription */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* 构造并发送目录信息 */
	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);
		Datum		values[3];
		bool		nulls[3] = {0};

		/* 发送一条数据行 */
		if (ti->path == NULL)
		{
			nulls[0] = true;
			nulls[1] = true;
		}
		else
		{
			values[0] = ObjectIdGetDatum(strtoul(ti->oid, NULL, 10));
			values[1] = CStringGetTextDatum(ti->path);
		}
		if (ti->size >= 0)
			values[2] = Int64GetDatum(ti->size / 1024);
		else
			nulls[2] = true;

		do_tup_output(tstate, values, nulls);
	}

	end_tup_output(tstate);
}
