/*-------------------------------------------------------------------------
 *
 * receivelog.c - receive WAL files using the streaming
 *				  replication protocol.
 *
 * Author: Magnus Hagander <magnus@hagander.net>
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/receivelog.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/file_utils.h"
#include "common/logging.h"
#include "libpq-fe.h"
#include "receivelog.h"
#include "streamutil.h"

/* 当前打开 WAL 文件 */
static Walfile *walfile = NULL;
static bool reportFlushPosition = false;
static XLogRecPtr lastFlushPosition = InvalidXLogRecPtr;

static bool still_sending = true;	/* 反馈仍需发送？            */

static PGresult *HandleCopyStream(PGconn *conn, StreamCtl *stream,
								  XLogRecPtr *stoppos);
static int	CopyStreamPoll(PGconn *conn, long timeout_ms, pgsocket stop_socket);
static int	CopyStreamReceive(PGconn *conn, long timeout, pgsocket stop_socket,
							  char **buffer);
static bool ProcessKeepaliveMsg(PGconn *conn, StreamCtl *stream, char *copybuf,
								int len, XLogRecPtr blockpos, TimestampTz *last_status);
static bool ProcessXLogDataMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
							   XLogRecPtr *blockpos);
static PGresult *HandleEndOfCopyStream(PGconn *conn, StreamCtl *stream, char *copybuf,
									   XLogRecPtr blockpos, XLogRecPtr *stoppos);
static bool CheckCopyStreamStop(PGconn *conn, StreamCtl *stream, XLogRecPtr blockpos);
static long CalculateCopyStreamSleeptime(TimestampTz now, int standby_message_timeout,
										 TimestampTz last_status);

static bool ReadEndOfStreamingResult(PGresult *res, XLogRecPtr *startpos,
									 uint32 *timeline);

static bool
mark_file_as_archived(StreamCtl *stream, const char *fname)
{
	Walfile    *f;
	static char tmppath[MAXPGPATH];

	snprintf(tmppath, sizeof(tmppath), "archive_status/%s.done",
			 fname);

	f = stream->walmethod->ops->open_for_write(stream->walmethod, tmppath,
											   NULL, 0);
	if (f == NULL)
	{
		pg_log_error("could not create archive status file \"%s\": %s",
					 tmppath, GetLastWalMethodError(stream->walmethod));
		return false;
	}

	if (stream->walmethod->ops->close(f, CLOSE_NORMAL) != 0)
	{
		pg_log_error("could not close archive status file \"%s\": %s",
					 tmppath, GetLastWalMethodError(stream->walmethod));
		return false;
	}

	return true;
}

/* * 在指定目录开新 WAL 文件。
 *
 * 成功 true；失败打印错返 false。
 * 成功 walfile 设为打开文件。
 *
 * 文件以 16MB 零填充。                                                                                 */
static bool
open_walfile(StreamCtl *stream, XLogRecPtr startpoint)
{
	Walfile    *f;
	char	   *fn;
	ssize_t		size;
	XLogSegNo	segno;
	char		walfile_name[MAXPGPATH];

	XLByteToSeg(startpoint, segno, WalSegSz);
	XLogFileName(walfile_name, stream->timeline, segno, WalSegSz);

	/* 注意必要时考虑压缩                                */
	fn = stream->walmethod->ops->get_file_name(stream->walmethod,
											   walfile_name,
											   stream->partial_suffix);

	/* * 流到文件验证已有为空(刚创)或
 * WalSegSz 完整段(已填充)。余=坏。
 * 压缩不需填充，略。
 *
 * 流到 tar 文件不先存，不验证。                                                                                                                                                                                                                                                               */
	if (stream->walmethod->compression_algorithm == PG_COMPRESSION_NONE &&
		stream->walmethod->ops->existsfile(stream->walmethod, fn))
	{
		size = stream->walmethod->ops->get_file_size(stream->walmethod, fn);
		if (size < 0)
		{
			pg_log_error("could not get size of write-ahead log file \"%s\": %s",
						 fn, GetLastWalMethodError(stream->walmethod));
			pg_free(fn);
			return false;
		}
		if (size == WalSegSz)
		{
			/* 已填充文件，打开用          */
			f = stream->walmethod->ops->open_for_write(stream->walmethod, walfile_name, stream->partial_suffix, 0);
			if (f == NULL)
			{
				pg_log_error("could not open existing write-ahead log file \"%s\": %s",
							 fn, GetLastWalMethodError(stream->walmethod));
				pg_free(fn);
				return false;
			}

			/* fsync 防前次崩溃                  */
			if (stream->walmethod->ops->sync(f) != 0)
			{
				pg_log_error("could not fsync existing write-ahead log file \"%s\": %s",
							 fn, GetLastWalMethodError(stream->walmethod));
				stream->walmethod->ops->close(f, CLOSE_UNLINK);
				exit(1);
			}

			walfile = f;
			pg_free(fn);
			return true;
		}
		if (size != 0)
		{
			/* write 未设 errno 假设磁盘不足                      */
			if (errno == 0)
				errno = ENOSPC;
			pg_log_error(ngettext("write-ahead log file \"%s\" has %zd byte, should be 0 or %d",
								  "write-ahead log file \"%s\" has %zd bytes, should be 0 or %d",
								  size),
						 fn, size, WalSegSz);
			pg_free(fn);
			return false;
		}
		/* File existed and was empty, so fall through and open */
	}

	/* 无文件，新建             */

	f = stream->walmethod->ops->open_for_write(stream->walmethod,
											   walfile_name,
											   stream->partial_suffix,
											   WalSegSz);
	if (f == NULL)
	{
		pg_log_error("could not open write-ahead log file \"%s\": %s",
					 fn, GetLastWalMethodError(stream->walmethod));
		pg_free(fn);
		return false;
	}

	pg_free(fn);
	walfile = f;
	return true;
}

/* * 关当前 WAL 文件(若开)，完整则重
 * 命名。失败打印错返 false。                                                                                                            */
static bool
close_walfile(StreamCtl *stream, XLogRecPtr pos)
{
	char	   *fn;
	pgoff_t		currpos;
	int			r;
	char		walfile_name[MAXPGPATH];

	if (walfile == NULL)
		return true;

	strlcpy(walfile_name, walfile->pathname, MAXPGPATH);
	currpos = walfile->currpos;

	/* 注意必要时考虑压缩                                */
	fn = stream->walmethod->ops->get_file_name(stream->walmethod,
											   walfile_name,
											   stream->partial_suffix);

	if (stream->partial_suffix)
	{
		if (currpos == WalSegSz)
			r = stream->walmethod->ops->close(walfile, CLOSE_NORMAL);
		else
		{
			pg_log_info("not renaming \"%s\", segment is not complete", fn);
			r = stream->walmethod->ops->close(walfile, CLOSE_NO_RENAME);
		}
	}
	else
		r = stream->walmethod->ops->close(walfile, CLOSE_NORMAL);

	walfile = NULL;

	if (r != 0)
	{
		pg_log_error("could not close file \"%s\": %s",
					 fn, GetLastWalMethodError(stream->walmethod));

		pg_free(fn);
		return false;
	}

	pg_free(fn);

	/* * 按调用者要求标记归档——
 * pg_basebackup 需此防提升后再归档。
 * 与 walreceiver 的 XLogArchiveForceDone
 * 一致。                                                                                                                               */
	if (currpos == WalSegSz && stream->mark_done)
	{
		/* 失败写错误消息          */
		if (!mark_file_as_archived(stream, walfile_name))
			return false;
	}

	lastFlushPosition = pos;
	return true;
}


/*
 * Check if a timeline history file exists.
 */
static bool
existsTimeLineHistoryFile(StreamCtl *stream)
{
	char		histfname[MAXFNAMELEN];

	/* * 时间线 1 无历史文件，视为存在。
 *                                                               */
	if (stream->timeline == 1)
		return true;

	TLHistoryFileName(histfname, stream->timeline);

	return stream->walmethod->ops->existsfile(stream->walmethod, histfname);
}

static bool
writeTimeLineHistoryFile(StreamCtl *stream, char *filename, char *content)
{
	int			size = strlen(content);
	char		histfname[MAXFNAMELEN];
	Walfile    *f;

	/* * 检查服务器时间线历史命名是否
 * 匹配。                                          */
	TLHistoryFileName(histfname, stream->timeline);
	if (strcmp(histfname, filename) != 0)
	{
		pg_log_error("server reported unexpected history file name for timeline %u: %s",
					 stream->timeline, filename);
		return false;
	}

	f = stream->walmethod->ops->open_for_write(stream->walmethod,
											   histfname, ".tmp", 0);
	if (f == NULL)
	{
		pg_log_error("could not create timeline history file \"%s\": %s",
					 histfname, GetLastWalMethodError(stream->walmethod));
		return false;
	}

	if ((int) stream->walmethod->ops->write(f, content, size) != size)
	{
		pg_log_error("could not write timeline history file \"%s\": %s",
					 histfname, GetLastWalMethodError(stream->walmethod));

		/* * 创建失败则删释放空间
 *                                  */
		stream->walmethod->ops->close(f, CLOSE_UNLINK);

		return false;
	}

	if (stream->walmethod->ops->close(f, CLOSE_NORMAL) != 0)
	{
		pg_log_error("could not close file \"%s\": %s",
					 histfname, GetLastWalMethodError(stream->walmethod));
		return false;
	}

	/* 维护 archive_status，详见 close_walfile                */
	if (stream->mark_done)
	{
		/* 失败写错误消息          */
		if (!mark_file_as_archived(stream, histfname))
			return false;
	}

	return true;
}

/*
 * Send a Standby Status Update message to server.
 */
static bool
sendFeedback(PGconn *conn, XLogRecPtr blockpos, TimestampTz now, bool replyRequested)
{
	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(blockpos, &replybuf[len]); /* 写   */
	len += 8;
	if (reportFlushPosition)
		fe_sendint64(lastFlushPosition, &replybuf[len]);	/* 刷   */
	else
		fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* 刷   */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* 应   */
	len += 8;
	fe_sendint64(now, &replybuf[len]);	/* 发时   */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0; /* 复请         */
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

/* * 检查服务器版本是否被
 * ReceiveXlogStream 支持。不支持
 * 打印错返 false。                                                                             */
bool
CheckServerVersionForStreaming(PGconn *conn)
{
	int			minServerMajor,
				maxServerMajor;
	int			serverMajor;

	/* * 流复制消息格式 9.3 变，不能从
 * 旧服务器流。也不支持比客户端新
 * 的服务器。                                                                                                                           */
	minServerMajor = 903;
	maxServerMajor = PG_VERSION_NUM / 100;
	serverMajor = PQserverVersion(conn) / 100;
	if (serverMajor < minServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_log_error("incompatible server version %s; client does not support streaming from server versions older than %s",
					 serverver ? serverver : "'unknown'",
					 "9.3");
		return false;
	}
	else if (serverMajor > maxServerMajor)
	{
		const char *serverver = PQparameterStatus(conn, "server_version");

		pg_log_error("incompatible server version %s; client does not support streaming from server versions newer than %s",
					 serverver ? serverver : "'unknown'",
					 PG_VERSION);
		return false;
	}
	return true;
}

/* * 从指定位置收日志流。
 *
 * 参数通过 StreamCtl 传。
 *
 * sysidentifier 指定时验证系统标
 * 识和时间线。
 *
 * 段写 basedir 目录，获取缺时间线
 * 历史。
 *
 * stream_stop 回调收数据及段完成
 * 时调用。true 停 false 续。
 *
 * stream_stop 查外部输入设
 * stop_socket。信号可中断但有竞
 * 态。
 *
 * standby_message_timeout 控向主
 * 发进度频率(ms)。0=不发。仅写位
 * 置，无刷新/重放。
 *
 * partial_suffix 非 NULL 文件先
 * 带后缀，完后移除以分部分/完成。
 *
 * synchronous=true 写后即刷新，
 * 否则关时刷。
 *
 * 注意：WAL 位须段起始！                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         */
bool
ReceiveXlogStream(PGconn *conn, StreamCtl *stream)
{
	char		query[128];
	char		slotcmd[128];
	PGresult   *res;
	XLogRecPtr	stoppos;

	/* * 调用者应已查版本，再查无妨。
 *                                                                  */
	if (!CheckServerVersionForStreaming(conn))
		return false;

	/* * 决定是否报告刷新位。报告主知可
 * 能重请求的 WAL，安全删旧 WAL。
 * 用槽必须报告。
 *
 * 报告刷新使成同步副本候选。不应
 * 在 synchronous_standby_names 放
 * 通用名，但一直保护，继续。                                                                                                                                                                                                                                                                           */
	if (stream->replication_slot != NULL)
	{
		reportFlushPosition = true;
		sprintf(slotcmd, "SLOT \"%s\" ", stream->replication_slot);
	}
	else
	{
		if (stream->synchronous)
			reportFlushPosition = true;
		else
			reportFlushPosition = false;
		slotcmd[0] = 0;
	}

	if (stream->sysidentifier != NULL)
	{
		char	   *sysidentifier = NULL;
		TimeLineID	servertli;

		/* * 取服务器系统标识和时间线验证。
 *                        */
		if (!RunIdentifySystem(conn, &sysidentifier, &servertli, NULL, NULL))
		{
			pg_free(sysidentifier);
			return false;
		}

		if (strcmp(stream->sysidentifier, sysidentifier) != 0)
		{
			pg_log_error("system identifier does not match between base backup and streaming connection");
			pg_free(sysidentifier);
			return false;
		}
		pg_free(sysidentifier);

		if (stream->timeline > servertli)
		{
			pg_log_error("starting timeline %u is not present in the server",
						 stream->timeline);
			return false;
		}
	}

	/* * 刷新位初始为起点，调用者负责
 * 合理。                                                 */
	lastFlushPosition = stream->startpos;

	while (1)
	{
		/* * 取时间线历史(如无)。流到 tar 总
 * 返 false(无已有文件)。                                                                                                                                                                                                     */
		if (!existsTimeLineHistoryFile(stream))
		{
			snprintf(query, sizeof(query), "TIMELINE_HISTORY %u", stream->timeline);
			res = PQexec(conn, query);
			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				/* FIXME：可能发 OK 但收错               */
				pg_log_error("could not send replication command \"%s\": %s",
							 "TIMELINE_HISTORY", PQresultErrorMessage(res));
				PQclear(res);
				return false;
			}

			/* * TIMELINE_HISTORY 响应：单行两
 * 字段，filename 和 content                                            */
			if (PQnfields(res) != 2 || PQntuples(res) != 1)
			{
				pg_log_warning("unexpected response to TIMELINE_HISTORY command: got %d rows and %d fields, expected %d rows and %d fields",
							   PQntuples(res), PQnfields(res), 1, 2);
			}

			/* 写历史文件到磁盘       */
			writeTimeLineHistoryFile(stream,
									 PQgetvalue(res, 0, 0),
									 PQgetvalue(res, 0, 1));

			PQclear(res);
		}

		/* * 流传输前检回调是否需停。
 *                                                                        */
		if (stream->stream_stop(stream->startpos, stream->timeline, false))
			return true;

		/* 在指定位置启动复制流                        */
		snprintf(query, sizeof(query), "START_REPLICATION %s%X/%X TIMELINE %u",
				 slotcmd,
				 LSN_FORMAT_ARGS(stream->startpos),
				 stream->timeline);
		res = PQexec(conn, query);
		if (PQresultStatus(res) != PGRES_COPY_BOTH)
		{
			pg_log_error("could not send replication command \"%s\": %s",
						 "START_REPLICATION", PQresultErrorMessage(res));
			PQclear(res);
			return false;
		}
		PQclear(res);

		/* 流传输 WAL  */
		res = HandleCopyStream(conn, stream, &stoppos);
		if (res == NULL)
			goto error;

		/* * 流结束。
 *
 * 两因：受控关闭，或时间线结束。
 * 时间线结束服发下一时间线信息，
 * 读取后重新开始。受控关闭停。                                                                                                                                                                                                                                                    */
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			/* * 时间线结束。读下一时间线 ID 和
 * 起始。通常起始匹配前结束，边界
 * 如提升时发半个 WAL 记录，新时
 * 间线从旧最后完整记录末开始。                                                                                                                                                                                                                                                    */
			uint32		newtimeline;
			bool		parsed;

			parsed = ReadEndOfStreamingResult(res, &stream->startpos, &newtimeline);
			PQclear(res);
			if (!parsed)
				goto error;

			/* 健全检查服务器返回值             */
			if (newtimeline <= stream->timeline)
			{
				pg_log_error("server reported unexpected next timeline %u, following timeline %u",
							 newtimeline, stream->timeline);
				goto error;
			}
			if (stream->startpos > stoppos)
			{
				pg_log_error("server stopped streaming timeline %u at %X/%X, but reported next timeline %u to begin at %X/%X",
							 stream->timeline, LSN_FORMAT_ARGS(stoppos),
							 newtimeline, LSN_FORMAT_ARGS(stream->startpos));
				goto error;
			}

			/* 读最终结果，应为 CommandComplete                */
			res = PQgetResult(conn);
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				pg_log_error("unexpected termination of replication stream: %s",
							 PQresultErrorMessage(res));
				PQclear(res);
				goto error;
			}
			PQclear(res);

			/* * 循环从新时间线流。始终段起始。
 *                                                                        */
			stream->timeline = newtimeline;
			stream->startpos = stream->startpos -
				XLogSegmentOffset(stream->startpos, WalSegSz);
			continue;
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/* * 复制结束（服受控关闭）。
 *
 * 检查回调是否允停，否则投诉。                                                                       */
			if (stream->stream_stop(stoppos, stream->timeline, false))
				return true;
			else
			{
				pg_log_error("replication stream was terminated before stop point");
				goto error;
			}
		}
		else
		{
			/* 服务器返回错误     */
			pg_log_error("unexpected termination of replication stream: %s",
						 PQresultErrorMessage(res));
			PQclear(res);
			goto error;
		}
	}

error:
	if (walfile != NULL && stream->walmethod->ops->close(walfile, CLOSE_NO_RENAME) != 0)
		pg_log_error("could not close file \"%s\": %s",
					 walfile->pathname, GetLastWalMethodError(stream->walmethod));
	walfile = NULL;
	return false;
}

/* * 解析流结束后服务器返结果集辅助
 * 函数。失败打印错返 false。                                                                  */
static bool
ReadEndOfStreamingResult(PGresult *res, XLogRecPtr *startpos, uint32 *timeline)
{
	uint32		startpos_xlogid,
				startpos_xrecoff;

	/*----------
	 * The result set consists of one row and two columns, e.g:
	 *
	 *	next_tli | next_tli_startpos
	 * ----------+-------------------
	 *		   4 | 0/9949AE0
	 *
	 * next_tli is the timeline ID of the next timeline after the one that
	 * just finished streaming. next_tli_startpos is the WAL location where
	 * the server switched to it.
	 *----------
	 */
	if (PQnfields(res) < 2 || PQntuples(res) != 1)
	{
		pg_log_error("unexpected result set after end-of-timeline: got %d rows and %d fields, expected %d rows and %d fields",
					 PQntuples(res), PQnfields(res), 1, 2);
		return false;
	}

	*timeline = atoi(PQgetvalue(res, 0, 0));
	if (sscanf(PQgetvalue(res, 0, 1), "%X/%X", &startpos_xlogid,
			   &startpos_xrecoff) != 2)
	{
		pg_log_error("could not parse next timeline's starting point \"%s\"",
					 PQgetvalue(res, 0, 1));
		return false;
	}
	*startpos = ((uint64) startpos_xlogid << 32) | startpos_xrecoff;

	return true;
}

/* * ReceiveXlogStream 主循环。
 * START_REPLICATION 后处理 COPY。
 *
 * COPY 因服消息结束返 PGresult
 * 并设 *stoppos。他错返 NULL。                                                                                                                                                                              */
static PGresult *
HandleCopyStream(PGconn *conn, StreamCtl *stream,
				 XLogRecPtr *stoppos)
{
	char	   *copybuf = NULL;
	TimestampTz last_status = -1;
	XLogRecPtr	blockpos = stream->startpos;

	still_sending = true;

	while (1)
	{
		int			r;
		TimestampTz now;
		long		sleeptime;

		/* * 检查继续流还是中止。
 *                                    */
		if (!CheckCopyStreamStop(conn, stream, blockpos))
			goto error;

		now = feGetCurrentTimestamp();

		/* * synchronous=true 有未刷新
 * WAL 即 sync。                                                                           */
		if (stream->synchronous && lastFlushPosition < blockpos && walfile != NULL)
		{
			if (stream->walmethod->ops->sync(walfile) != 0)
				pg_fatal("could not fsync file \"%s\": %s",
						 walfile->pathname, GetLastWalMethodError(stream->walmethod));
			lastFlushPosition = blockpos;

			/* * 发反馈让服立见最新 WAL。
 *                                                     */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/* * 可能向主发状态
 *                               */
		if (still_sending && stream->standby_message_timeout > 0 &&
			feTimestampDifferenceExceeds(last_status, now,
										 stream->standby_message_timeout))
		{
			/* 该发反馈！        */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/* * 计算收发循环休眠时长
 *                        */
		sleeptime = CalculateCopyStreamSleeptime(now, stream->standby_message_timeout,
												 last_status);

		r = CopyStreamReceive(conn, sleeptime, stream->stop_socket, &copybuf);
		while (r != 0)
		{
			if (r == -1)
				goto error;
			if (r == -2)
			{
				PGresult   *res = HandleEndOfCopyStream(conn, stream, copybuf, blockpos, stoppos);

				if (res == NULL)
					goto error;
				else
					return res;
			}

			/* 检查消息类型      */
			if (copybuf[0] == 'k')
			{
				if (!ProcessKeepaliveMsg(conn, stream, copybuf, r, blockpos,
										 &last_status))
					goto error;
			}
			else if (copybuf[0] == 'w')
			{
				if (!ProcessXLogDataMsg(conn, stream, copybuf, r, &blockpos))
					goto error;

				/* * 检查继续流还是中止。
 *                                               */
				if (!CheckCopyStreamStop(conn, stream, blockpos))
					goto error;
			}
			else
			{
				pg_log_error("unrecognized streaming header: \"%c\"",
							 copybuf[0]);
				goto error;
			}

			/* * 处理收到数据及后续非阻塞数据。
 *                                               */
			r = CopyStreamReceive(conn, 0, stream->stop_socket, &copybuf);
		}
	}

error:
	PQfreemem(copybuf);
	return NULL;
}

/* * 等 CopyData 可读、超时或信号/
 * stop_socket 输入。(<0 等;0 不)
 *
 * 返 1=可读 0=超时/中断 -1=错。                                                                                                                                                                                                         */
static int
CopyStreamPoll(PGconn *conn, long timeout_ms, pgsocket stop_socket)
{
	int			ret;
	fd_set		input_mask;
	int			connsocket;
	int			maxfd;
	struct timeval timeout;
	struct timeval *timeoutptr;

	connsocket = PQsocket(conn);
	if (connsocket < 0)
	{
		pg_log_error("invalid socket: %s", PQerrorMessage(conn));
		return -1;
	}

	FD_ZERO(&input_mask);
	FD_SET(connsocket, &input_mask);
	maxfd = connsocket;
	if (stop_socket != PGINVALID_SOCKET)
	{
		FD_SET(stop_socket, &input_mask);
		maxfd = Max(maxfd, stop_socket);
	}

	if (timeout_ms < 0)
		timeoutptr = NULL;
	else
	{
		timeout.tv_sec = timeout_ms / 1000L;
		timeout.tv_usec = (timeout_ms % 1000L) * 1000L;
		timeoutptr = &timeout;
	}

	ret = select(maxfd + 1, &input_mask, NULL, NULL, timeoutptr);

	if (ret < 0)
	{
		if (errno == EINTR)
			return 0;			/* 信号，非错               */
		pg_log_error("%s() failed: %m", "select");
		return -1;
	}
	if (ret > 0 && FD_ISSET(connsocket, &input_mask))
		return 1;				/* 连接套接字有输入       */

	return 0;					/* 超时或 stop_socket 输入        */
}

/* * 从 XLOG 流收 CopyData，最多阻
 * 塞 timeout ms。
 *
 * 收到返长度，*buffer 指消息缓冲
 * (仅下次 CopyStreamReceive 前有效)。
 *
 * 超时/中断返 0。错 -1。服结束
 * COPY 返 -2。                                                                                                                                                                                                                                                     */
static int
CopyStreamReceive(PGconn *conn, long timeout, pgsocket stop_socket,
				  char **buffer)
{
	char	   *copybuf = NULL;
	int			rawlen;

	PQfreemem(*buffer);
	*buffer = NULL;

	/* 尝试收 CopyData                */
	rawlen = PQgetCopyData(conn, &copybuf, 1);
	if (rawlen == 0)
	{
		int			ret;

		/* * 无数据。等 timeout 内出以便
 * ping 服务器，stop_socket 有输
 * 入也停。                                                                                                 */
		ret = CopyStreamPoll(conn, timeout, stop_socket);
		if (ret <= 0)
			return ret;

		/* 套接字确有数据                    */
		if (PQconsumeInput(conn) == 0)
		{
			pg_log_error("could not receive data from WAL stream: %s",
						 PQerrorMessage(conn));
			return -1;
		}

		/* 已消费输入，再试                      */
		rawlen = PQgetCopyData(conn, &copybuf, 1);
		if (rawlen == 0)
			return 0;
	}
	if (rawlen == -1)			/* 流结束或错误        */
		return -2;
	if (rawlen == -2)
	{
		pg_log_error("could not read COPY data: %s", PQerrorMessage(conn));
		return -1;
	}

	/* 返收到消息给调用者        */
	*buffer = copybuf;
	return rawlen;
}

/*
 * Process the keepalive message.
 */
static bool
ProcessKeepaliveMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
					XLogRecPtr blockpos, TimestampTz *last_status)
{
	int			pos;
	bool		replyRequested;
	TimestampTz now;

	/* * 解析 CopyData 内 keepalive。
 * 仅查是否求回复，略其余。                                                                   */
	pos = 1;					/* 跳 msgtype k    */
	pos += 8;					/* 跳 walEnd  */
	pos += 8;					/* 跳 sendTime  */

	if (len < pos + 1)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	replyRequested = copybuf[pos];

	/* 服求即复则发                                    */
	if (replyRequested && still_sending)
	{
		if (reportFlushPosition && lastFlushPosition < blockpos &&
			walfile != NULL)
		{
			/* * 需报有效刷新位，刷新当前 WAL
 * 回传最新位，供服正常关时确认。                                                                                                                                                                                                                  */
			if (stream->walmethod->ops->sync(walfile) != 0)
				pg_fatal("could not fsync file \"%s\": %s",
						 walfile->pathname, GetLastWalMethodError(stream->walmethod));
			lastFlushPosition = blockpos;
		}

		now = feGetCurrentTimestamp();
		if (!sendFeedback(conn, blockpos, now, false))
			return false;
		*last_status = now;
	}

	return true;
}

/*
 * Process XLogData message.
 */
static bool
ProcessXLogDataMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
				   XLogRecPtr *blockpos)
{
	int			xlogoff;
	int			bytes_left;
	int			bytes_written;
	int			hdr_len;

	/* * 决定不再收后略后续 XLogData。
 *                                                                  */
	if (!(still_sending))
		return true;

	/* * 读 XLogData 头(CopyData 内)。
 * 仅需 WAL 位(dataStart)，余略。                                                                                                 */
	hdr_len = 1;				/* msgtype w   */
	hdr_len += 8;				/* 数据起 */
	hdr_len += 8;				/* walEnd */
	hdr_len += 8;				/* 发时   */
	if (len < hdr_len)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	*blockpos = fe_recvint64(&copybuf[1]);

	/* 提取此块 WAL 位                */
	xlogoff = XLogSegmentOffset(*blockpos, WalSegSz);

	/* * 验证流初始位匹配预期。
 *                                                  */
	if (walfile == NULL)
	{
		/* 文件未开     */
		if (xlogoff != 0)
		{
			pg_log_error("received write-ahead log record for offset %u with no file open",
						 xlogoff);
			return false;
		}
	}
	else
	{
		/* 现有段更多数据         */
		if (walfile->currpos != xlogoff)
		{
			pg_log_error("got WAL data offset %08x, expected %08x",
						 xlogoff, (int) walfile->currpos);
			return false;
		}
	}

	bytes_left = len - hdr_len;
	bytes_written = 0;

	while (bytes_left)
	{
		int			bytes_to_write;

		/* * 跨 WAL 边界只写到段大小。
 *                                                */
		if (xlogoff + bytes_left > WalSegSz)
			bytes_to_write = WalSegSz - xlogoff;
		else
			bytes_to_write = bytes_left;

		if (walfile == NULL)
		{
			if (!open_walfile(stream, *blockpos))
			{
				/* open_walfile 已记错       */
				return false;
			}
		}

		if (stream->walmethod->ops->write(walfile,
										  copybuf + hdr_len + bytes_written,
										  bytes_to_write) != bytes_to_write)
		{
			pg_log_error("could not write %d bytes to WAL file \"%s\": %s",
						 bytes_to_write, walfile->pathname,
						 GetLastWalMethodError(stream->walmethod));
			return false;
		}

		/* 写成功，推进位                      */
		bytes_written += bytes_to_write;
		bytes_left -= bytes_to_write;
		*blockpos += bytes_to_write;
		xlogoff += bytes_to_write;

		/* 到 WAL 段末？                      */
		if (XLogSegmentOffset(*blockpos, WalSegSz) == 0)
		{
			if (!close_walfile(stream, *blockpos))
				/* close_walfile 已写错                  */
				return false;

			xlogoff = 0;

			if (still_sending && stream->stream_stop(*blockpos, stream->timeline, true))
			{
				if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
				{
					pg_log_error("could not send copy-end packet: %s",
								 PQerrorMessage(conn));
					return false;
				}
				still_sending = false;
				return true;	/* 略此 XLogData 包余                  */
			}
		}
	}
	/* 无更多数据，收下一 copy 包                 */

	return true;
}

/*
 * Handle end of the copy stream.
 */
static PGresult *
HandleEndOfCopyStream(PGconn *conn, StreamCtl *stream, char *copybuf,
					  XLogRecPtr blockpos, XLogRecPtr *stoppos)
{
	PGresult   *res = PQgetResult(conn);

	/* * 服关 copy 端。未关则关(除
 * 非服抛错)。                                                                                                                        */
	if (still_sending)
	{
		if (!close_walfile(stream, blockpos))
		{
			/* close_walfile 已写错                  */
			PQclear(res);
			return NULL;
		}
		if (PQresultStatus(res) == PGRES_COPY_IN)
		{
			if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
			{
				pg_log_error("could not send copy-end packet: %s",
							 PQerrorMessage(conn));
				PQclear(res);
				return NULL;
			}
			res = PQgetResult(conn);
		}
		still_sending = false;
	}
	PQfreemem(copybuf);
	*stoppos = blockpos;
	return res;
}

/* * 检查继续流还是中止。
 *                                */
static bool
CheckCopyStreamStop(PGconn *conn, StreamCtl *stream, XLogRecPtr blockpos)
{
	if (still_sending && stream->stream_stop(blockpos, stream->timeline, false))
	{
		if (!close_walfile(stream, blockpos))
		{
			/* close_walfile 写潜在错                          */
			return false;
		}
		if (PQputCopyEnd(conn, NULL) <= 0 || PQflush(conn))
		{
			pg_log_error("could not send copy-end packet: %s",
						 PQerrorMessage(conn));
			return false;
		}
		still_sending = false;
	}

	return true;
}

/* * 计算收发循环休眠时长
 *                    */
static long
CalculateCopyStreamSleeptime(TimestampTz now, int standby_message_timeout,
							 TimestampTz last_status)
{
	TimestampTz status_targettime = 0;
	long		sleeptime;

	if (standby_message_timeout && still_sending)
		status_targettime = last_status +
			(standby_message_timeout - 1) * ((int64) 1000);

	if (status_targettime > 0)
	{
		long		secs;
		int			usecs;

		feTimestampDifference(now,
							  status_targettime,
							  &secs,
							  &usecs);
		/* 始终至少休眠 1 秒    */
		if (secs <= 0)
		{
			secs = 1;
			usecs = 0;
		}

		sleeptime = secs * 1000 + usecs / 1000;
	}
	else
		sleeptime = -1;

	return sleeptime;
}
