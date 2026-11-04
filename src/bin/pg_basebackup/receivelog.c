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

/*
 * 在指定目录中打开新的 WAL 文件。   
 *
 * 成功时返回 true；失败时打印错误消息后返回 false。      
 * 成功时 'walfile' 设为打开的 WAL 文件。   
 *
 * 该文件将以零填充至 16MB。         
 */
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

	/*
	 * 流式传输到文件时，若已存在同名文件，则验证它是  
	 * 空文件（刚创建）或完整的 WalSegSz 段（此时它已被  
	 * 创建并填充过）。其他任何情况都表明文件已损坏。  
	 * 压缩文件无需填充，因此直接忽略这种情况。        
	 *      
	 *
	 * 流式传输到 tar 时，之前不存在该名称的文件，因此  
	 * 我们无需验证大小。 
	 */
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

/*
 * 关闭当前 WAL 文件（若打开），若完整则重命名为 
 * 正确的文件名。失败时打印错误消息到 stderr 并返回    
 * false，否则返回 true。              
 */
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

	/*
	 * 若调用者要求，则将文件标记为已归档 - pg_basebackup   
	 * 必须如此，否则新节点提升后文件可能再次被归档。  
	 * 这与 walreceiver.c 在完整段后总是执行           
	 * XLogArchiveForceDone() 的操作一致。       
	 */
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
 * 检查时间线历史文件的有无。 
 */
static bool
existsTimeLineHistoryFile(StreamCtl *stream)
{
	char		histfname[MAXFNAMELEN];

	/*
	 * 时间线 1 从无历史文件。我们将其视作已存在，     
	 * 因无需流式传输它。      
	 */
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

	/*
	 * 检查服务器对时间线历史文件的命名方式是否        
	 * 与我们一致。 
	 */
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

		/*
		 * 若创建文件失败，则删除它以释放磁盘空间   
		 */
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
 * 向服务器发送备库状态更新消息。  
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
	replybuf[len] = replyRequested ? 1 : 0; /* replyRequested */
	len += 1;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

/*
 * 检查所连接的服务器版本是否受 ReceiveXlogStream()  
 * 的支持。        
 *
 * 如果不支持，则向 stderr 打印错误消息并返回 false。      
 */
bool
CheckServerVersionForStreaming(PGconn *conn)
{
	int			minServerMajor,
				maxServerMajor;
	int			serverMajor;

	/*
	 * 流复制使用的消息格式在 9.3 发生了变化，因此我们  
	 * 不能从旧服务器流式复制。且不支持比客户端更新  
	 * 的服务器；它也许能行，但不确定，稳妥为上。      
	 *      
	 */
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

/*
 * 从指定位置开始接收一个日志流。           
 *
 * 各个参数通过 StreamCtl 结构传递。                     
 *
 * 如果指定了 sysidentifier，则验证系统标识符和 
 * 时间线是否均与指定值相符（通过发送   
 * IDENTIFY_SYSTEM 命令进行验证）        
 *
 * 接收到的所有段都会写入 basedir 指定的   
 * 目录中。这也会获取任何缺失的时间线历史文件。     
 *       
 *
 * stream_stop 回调会在每次收到数据时以及    
 * 每当一个段完成时被调用。若它返回 true，       
 * 流式传输就停止，函数返回。只要 
 * 它返回 false，流式传输就会无限期持续。      
 *              
 *
 * 如果 stream_stop() 会检查外部输入，则应将 stop_socket      
 * 设置为其检查的 FD。这样此类输入就能被及时发现，
 * 而不必等到 standby_message_timeout（它可能是                
 * 无限期的）。注意信号也会中断对输入的等待，      
 * 但这有竞态：忙碌时收到的信号不会中断等待。    
 *
 * standby_message_timeout 控制我们向主库发            
 * 送消息告知自身进度（以毫秒计）的频率。零表示
 * 不发送任何消息。        
 * 该消息将只包含写入位置，而绝不包含刷新或
 * 重放位置。 
 *
 * 如果 'partial_suffix' 不为 NULL，文件最初会以给定       
 * 的后缀创建，并且文件完成后移除该后缀。这使得你  
 * 能够区分部分文件与已完成的文件，以便你以后可以 
 * 从离开的地方继续。                   
 *
 * 如果 'synchronous' 为 true，则接收到的 WAL 会在写入后      
 * 立即刷新，否则关闭时才刷新。 
 *
 * 注意：WAL 位置*必须*在日志段起点！        
 */
bool
ReceiveXlogStream(PGconn *conn, StreamCtl *stream)
{
	char		query[128];
	char		slotcmd[128];
	PGresult   *res;
	XLogRecPtr	stoppos;

	/*
	 * 调用者应已检查过服务器版本，但在这里再检查        
	 * 一次也无妨。            
	 */
	if (!CheckServerVersionForStreaming(conn))
		return false;

	/*
	 * 决定我们是否要报告刷新位置。若报告刷新位置，   
	 * 主库就会知道我们可能重新请求哪些 WAL，      
	 * 从而可以安全地移除较旧的 WAL。在使用复制槽时我们
	 * 必须始终这样做。     
	 *
	 * 报告刷新位置使我们具备同步备库资格。          
	 * 人们不应在 synchronous_standby_names         
	 * 中放入通用名称，但迄今我们一直对此加以保护，    
	 * 因此除非被明确要求，否则将继续这样做。
	 */
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

		/*
		 * 获取服务器 system_identifier 与时间线并验证。       
		 */
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

	/*
	 * 把刷新位置初始化为起点，是否合理由调用者  
	 * 负责保证。                 
	 */
	lastFlushPosition = stream->startpos;

	while (1)
	{
		/*
		 * 获取此时间线的时间线历史文件（若尚未持有       
		 * 该文件）。流式传输到 tar 时总会返回 false，   
		 * 因为我们绝不会流式写入已存在的文件，    
		 * 因此不可能有预先存在的时间线历史文件。    
		 */
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

			/*
			 * 对 TIMELINE_HISTORY 的响应是带有两个字段的     
			 * 单行结果集：filename、content 
			 */
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

		/*
		 * 在从请求的位置开始流式传输之前，检查回调是否 
		 * 要求我们在此处停止。 
		 */
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

		/*
		 * 流已结束。    
		 *
		 * 这有两种可能的原因：受控关闭，或者到达了      
		 * 当前时间线末尾。若是时间线结束，      
		 * 服务器在 COPY 结束后发送包含下一时间线信息 
		 * 的结果集。读取它，并从下一时间线重新开始  
		 * 传输。若是受控关闭，则在此处停止。           
		 *                                
		 */
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
		{
			/*
			 * 时间线结束。读下一时间线 ID 和起始位置  
			 * 通常，起始位置与前一个时间线的末尾相同，  
			 * 但也有边界情况，例如服务器提升时发来了    
			 * 半个 WAL 记录。此时，新时间线将始于最后一条
			 * 完整记录的末尾，并重叠旧时间线上的        
			 * 部分 WAL 记录。                                          
			 *              
			 */
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

			/*
			 * 循环返回，从新时间线开始流式传输。始终 
			 * 在段的开头开始流式传输。          
			 */
			stream->timeline = newtimeline;
			stream->startpos = stream->startpos -
				XLogSegmentOffset(stream->startpos, WalSegSz);
			continue;
		}
		else if (PQresultStatus(res) == PGRES_COMMAND_OK)
		{
			PQclear(res);

			/*
			 * 复制结束（即服务器受控关闭）。               
			 *
			 * 检查回调是否认可停在此处。若否则报错。 
			 *          
			 */
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

/*
 * 流式传输结束后解析服务器所返回结果集的辅助函数。  
 * 失败时向 stderr 打印错误并返回 false。                    
 */
static bool
ReadEndOfStreamingResult(PGresult *res, XLogRecPtr *startpos, uint32 *timeline)
{
	uint32		startpos_xlogid,
				startpos_xrecoff;

	/*----------
	 * 结果集由一行和两列组成，例如：           
	 *
	 *	next_tli | next_tli_startpos
	 * ----------+-------------------
	 *		   4 | 0/9949AE0
	 *
	 * next_tli 是刚结束流式传输的时间线之后的下一个    
	 * 时间线的时间线 ID。next_tli_startpos 是服务器切换     
	 * 到它时的 WAL 位置。
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

/*
 * ReceiveXlogStream 的主循环。在以 START_REPLICATION        
 * 命令发起流式传输后，处理 COPY 流。        
 *
 * 若 COPY 因来自服务器的消息而结束（不一定成功）， 
 * 则返回一个 PGresult，并将 *stoppos 设为最后写入         
 * 的字节。其他错误均返回 NULL。
 */
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

		/*
		 * 检查此时应当继续流式传输，还是在此中止。  
		 */
		if (!CheckCopyStreamStop(conn, stream, blockpos))
			goto error;

		now = feGetCurrentTimestamp();

		/*
		 * 如果 synchronous 选项为 true，则一旦有尚未刷新的   
		 * WAL 数据，就立即发出 sync 命令。 
		 */
		if (stream->synchronous && lastFlushPosition < blockpos && walfile != NULL)
		{
			if (stream->walmethod->ops->sync(walfile) != 0)
				pg_fatal("could not fsync file \"%s\": %s",
						 walfile->pathname, GetLastWalMethodError(stream->walmethod));
			lastFlushPosition = blockpos;

			/*
			 * 发送反馈，让服务器立即看到最新的 WAL 位置。
			 *             
			 */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/*
		 * 可能向主库发送一条状态消息         
		 */
		if (still_sending && stream->standby_message_timeout > 0 &&
			feTimestampDifferenceExceeds(last_status, now,
										 stream->standby_message_timeout))
		{
			/* 该发反馈！        */
			if (!sendFeedback(conn, blockpos, now, false))
				goto error;
			last_status = now;
		}

		/*
		 * 计算发送/接收循环应当休眠多长时间 
		 */
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

				/*
				 * 检查此时应否继续流式传输还是中止。    
				 *       
				 */
				if (!CheckCopyStreamStop(conn, stream, blockpos))
					goto error;
			}
			else
			{
				pg_log_error("unrecognized streaming header: \"%c\"",
							 copybuf[0]);
				goto error;
			}

			/*
			 * 处理收到的数据，以及不阻塞即可读取的后续  
			 * 数据。        
			 */
			r = CopyStreamReceive(conn, 0, stream->stop_socket, &copybuf);
		}
	}

error:
	PQfreemem(copybuf);
	return NULL;
}

/*
 * 等待直到可以读取 CopyData         
 * 消息，或者发生超时、信号或 stop_socket 上的输入  
 * （timeout_ms < 0 表示无限等待；0 则不等待。）  
 *
 * 若有数据可读取则返回 1；若超时，或被信号或    
 * stop_socket 输入中断则返回 0；出错则返回 -1。       
 */
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

/*
 * 从 XLOG 流中接收可用的 CopyData 消息，最多阻塞    
 * 'timeout' 毫秒。     
 *
 * 如果收到了数据，则返回数据的长度。*buffer 被         
 * 设置为指向保存所收到消息的缓冲区。该缓冲区只在   
 * 下次 CopyStreamReceive 前有效。 
 *
 * 如果超时时间内没有数据可用，或者等待被信号  
 * 或 stop_socket 输入中断则返回 0。 
 * -1 出错。-2 服务器结束 COPY。       
 */
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

		/*
		 * 无可用数据。等待数据出现，但不能超过指定的 
		 * 超时时间，这样我们就能 ping 服务器。如果        
		 * stop_socket 有输入也停止等待。 
		 */
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
 * 处理 keepalive 消息。    
 */
static bool
ProcessKeepaliveMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
					XLogRecPtr blockpos, TimestampTz *last_status)
{
	int			pos;
	bool		replyRequested;
	TimestampTz now;

	/*
	 * 解析包含在 CopyData 消息中的 keepalive 消息。我们       
	 * 只检查服务器是否要求了回复，其余忽略。  
	 */
	pos = 1;					/* 跳 msgtype k    */
	pos += 8;					/* 跳 walEnd  */
	pos += 8;					/* 跳 sendTime  */

	if (len < pos + 1)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	replyRequested = copybuf[pos];

	/* 若服务器要求立即回复，则发送。         */
	if (replyRequested && still_sending)
	{
		if (reportFlushPosition && lastFlushPosition < blockpos &&
			walfile != NULL)
		{
			/*
			 * 如果需要报告有效的刷新位置，则刷新当前
			 * WAL 文件，以将最新刷新位置回送服务器。这是  
			 * 为了在服务器正常关闭时，确认最后的         
			 * WAL 数据是否已被成功复制。                      
			 *                        
			 */
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
 * 处理 XLogData 消息。
 */
static bool
ProcessXLogDataMsg(PGconn *conn, StreamCtl *stream, char *copybuf, int len,
				   XLogRecPtr *blockpos)
{
	int			xlogoff;
	int			bytes_left;
	int			bytes_written;
	int			hdr_len;

	/*
	 * 一旦我们决定不再接收更多数据，就直接忽略后续的
	 * XLogData 消息。           
	 */
	if (!(still_sending))
		return true;

	/*
	 * 读取包含在 CopyData 消息中的 XLogData 消息头部。   
	 * 我们只需要 WAL 位置字段（dataStart），头部的          
	 * 其余部分忽略。 
	 */
	hdr_len = 1;				/* 类型 'w'  */
	hdr_len += 8;				/* dataStart */
	hdr_len += 8;				/* walEnd */
	hdr_len += 8;				/* 发时   */
	if (len < hdr_len)
	{
		pg_log_error("streaming header too small: %d", len);
		return false;
	}
	*blockpos = fe_recvint64(&copybuf[1]);

	/* 提取此块的 WAL 位置          */
	xlogoff = XLogSegmentOffset(*blockpos, WalSegSz);

	/*
	 * 验证流中的初始位置是否与我们所认为的位置相符。
	 *        
	 */
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

		/*
		 * 如果跨越 WAL 边界，只写到 wal 段大小为止。  
		 *              
		 */
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

		/* 写入成功，推进我们的位置       */
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
				return true;	/* 忽略此 XLogData 包的其余部分   */
			}
		}
	}
	/* 无更多数据，收下一 copy 包                 */

	return true;
}

/*
 * 处理复制流的结束。   
 */
static PGresult *
HandleEndOfCopyStream(PGconn *conn, StreamCtl *stream, char *copybuf,
					  XLogRecPtr blockpos, XLogRecPtr *stoppos)
{
	PGresult   *res = PQgetResult(conn);

	/*
	 * 服务器已关闭复制流上自己的一端。如果我们       
	 * 还没有关闭自己这一端，就需要现在关闭。但         
	 * 抛错时除外。     
	 */
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

/*
 * 检查此时应当继续流式传输，还是在此中止。  
 */
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

/*
 * 计算发送/接收循环应当休眠多长时间 
 */
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
