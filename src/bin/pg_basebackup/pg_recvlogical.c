/*-------------------------------------------------------------------------
 *
 * pg_recvlogical.c - receive data from a logical decoding slot in a streaming
 *					  fashion and write it to a local file.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/pg_recvlogical.c
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <dirent.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/option_utils.h"
#include "getopt_long.h"
#include "libpq-fe.h"
#include "libpq/pqsignal.h"
#include "pqexpbuffer.h"
#include "streamutil.h"

/* 重连间隔休眠时间                    */
#define RECONNECT_SLEEP_TIME 5

/* 全局选项   */
static char *outfile = NULL;
static int	verbose = 0;
static bool two_phase = false;
static int	noloop = 0;
static int	standby_message_timeout = 10 * 1000;	/* 10s 默认       */
static int	fsync_interval = 10 * 1000; /* 10s 默认       */
static XLogRecPtr startpos = InvalidXLogRecPtr;
static XLogRecPtr endpos = InvalidXLogRecPtr;
static bool do_create_slot = false;
static bool slot_exists_ok = false;
static bool do_start_slot = false;
static bool do_drop_slot = false;
static char *replication_slot = NULL;

/* 配对 option,value。value 可 NULL                  */
static char **options;
static size_t noptions = 0;
static const char *plugin = "test_decoding";

/* 全局状态 */
static int	outfd = -1;
static volatile sig_atomic_t time_to_abort = false;
static volatile sig_atomic_t output_reopen = false;
static bool output_isfile;
static TimestampTz output_last_fsync = -1;
static bool output_needs_fsync = false;
static XLogRecPtr output_written_lsn = InvalidXLogRecPtr;
static XLogRecPtr output_fsync_lsn = InvalidXLogRecPtr;

static void usage(void);
static void StreamLogicalLog(void);
static bool flushAndSendFeedback(PGconn *conn, TimestampTz *now);
static void prepareToTerminate(PGconn *conn, XLogRecPtr endpos,
							   bool keepalive, XLogRecPtr lsn);

static void
usage(void)
{
	printf(_("%s controls PostgreSQL logical decoding streams.\n\n"),
		   progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nAction to be performed:\n"));
	printf(_("      --create-slot      create a new replication slot (for the slot's name see --slot)\n"));
	printf(_("      --drop-slot        drop the replication slot (for the slot's name see --slot)\n"));
	printf(_("      --start            start streaming in a replication slot (for the slot's name see --slot)\n"));
	printf(_("\nOptions:\n"));
	printf(_("  -E, --endpos=LSN       exit after receiving the specified LSN\n"));
	printf(_("  -f, --file=FILE        receive log into this file, - for stdout\n"));
	printf(_("  -F  --fsync-interval=SECS\n"
			 "                         time between fsyncs to the output file (default: %d)\n"), (fsync_interval / 1000));
	printf(_("      --if-not-exists    do not error if slot already exists when creating a slot\n"));
	printf(_("  -I, --startpos=LSN     where in an existing slot should the streaming start\n"));
	printf(_("  -n, --no-loop          do not loop on connection lost\n"));
	printf(_("  -o, --option=NAME[=VALUE]\n"
			 "                         pass option NAME with optional value VALUE to the\n"
			 "                         output plugin\n"));
	printf(_("  -P, --plugin=PLUGIN    use output plugin PLUGIN (default: %s)\n"), plugin);
	printf(_("  -s, --status-interval=SECS\n"
			 "                         time between status packets sent to server (default: %d)\n"), (standby_message_timeout / 1000));
	printf(_("  -S, --slot=SLOTNAME    name of the logical replication slot\n"));
	printf(_("  -t, --two-phase        enable decoding of prepared transactions when creating a slot\n"));
	printf(_("  -v, --verbose          output verbose messages\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nConnection options:\n"));
	printf(_("  -d, --dbname=DBNAME    database to connect to\n"));
	printf(_("  -h, --host=HOSTNAME    database server host or socket directory\n"));
	printf(_("  -p, --port=PORT        database server port number\n"));
	printf(_("  -U, --username=NAME    connect as specified database user\n"));
	printf(_("  -w, --no-password      never prompt for password\n"));
	printf(_("  -W, --password         force password prompt (should happen automatically)\n"));
	printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	printf(_("%s home page: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
}

/*
 * 向服务器发送 Standby Status Update 消息
 */
static bool
sendFeedback(PGconn *conn, TimestampTz now, bool force, bool replyRequested)
{
	static XLogRecPtr last_written_lsn = InvalidXLogRecPtr;
	static XLogRecPtr last_fsync_lsn = InvalidXLogRecPtr;

	char		replybuf[1 + 8 + 8 + 8 + 8 + 1];
	int			len = 0;

	/*
	 * 通常我们并不想发送多余的反馈，但如果是超时 
	 * 就必须发送，否则 wal_sender_timeout 会终止我们。         
	 *    
	 */
	if (!force &&
		last_written_lsn == output_written_lsn &&
		last_fsync_lsn == output_fsync_lsn)
		return true;

	if (verbose)
		pg_log_info("confirming write up to %X/%X, flush to %X/%X (slot %s)",
					LSN_FORMAT_ARGS(output_written_lsn),
					LSN_FORMAT_ARGS(output_fsync_lsn),
					replication_slot);

	replybuf[len] = 'r';
	len += 1;
	fe_sendint64(output_written_lsn, &replybuf[len]);	/* 写   */
	len += 8;
	fe_sendint64(output_fsync_lsn, &replybuf[len]); /* 刷   */
	len += 8;
	fe_sendint64(InvalidXLogRecPtr, &replybuf[len]);	/* 应   */
	len += 8;
	fe_sendint64(now, &replybuf[len]);	/* 发时   */
	len += 8;
	replybuf[len] = replyRequested ? 1 : 0; /* replyRequested */
	len += 1;

	startpos = output_written_lsn;
	last_written_lsn = output_written_lsn;
	last_fsync_lsn = output_fsync_lsn;

	if (PQputCopyData(conn, replybuf, len) <= 0 || PQflush(conn))
	{
		pg_log_error("could not send feedback packet: %s",
					 PQerrorMessage(conn));
		return false;
	}

	return true;
}

static void
disconnect_atexit(void)
{
	if (conn != NULL)
		PQfinish(conn);
}

static bool
OutputFsync(TimestampTz now)
{
	output_last_fsync = now;

	output_fsync_lsn = output_written_lsn;

	if (fsync_interval <= 0)
		return true;

	if (!output_needs_fsync)
		return true;

	output_needs_fsync = false;

	/* 仅常规文件可 fsync              */
	if (!output_isfile)
		return true;

	if (fsync(outfd) != 0)
		pg_fatal("could not fsync file \"%s\": %m", outfile);

	return true;
}

/*
 * 开始日志流接收  
 */
static void
StreamLogicalLog(void)
{
	PGresult   *res;
	char	   *copybuf = NULL;
	TimestampTz last_status = -1;
	int			i;
	PQExpBuffer query;

	output_written_lsn = InvalidXLogRecPtr;
	output_fsync_lsn = InvalidXLogRecPtr;

	/*
	 * 以复制模式连接到服务器。     
	 */
	if (!conn)
		conn = GetConnection();
	if (!conn)
		/* 错误已在 GetConnection 写入                */
		return;

	/*
	 * 开始复制。      
	 */
	if (verbose)
		pg_log_info("starting log streaming at %X/%X (slot %s)",
					LSN_FORMAT_ARGS(startpos),
					replication_slot);

	/* 在指定位置启动复制流                        */
	query = createPQExpBuffer();
	appendPQExpBuffer(query, "START_REPLICATION SLOT \"%s\" LOGICAL %X/%X",
					  replication_slot, LSN_FORMAT_ARGS(startpos));

	/* 有选项则打印             */
	if (noptions)
		appendPQExpBufferStr(query, " (");

	for (i = 0; i < noptions; i++)
	{
		/* 分隔    */
		if (i > 0)
			appendPQExpBufferStr(query, ", ");

		/* 写选项名      */
		appendPQExpBuffer(query, "\"%s\"", options[(i * 2)]);

		/* 有选项值则写              */
		if (options[(i * 2) + 1] != NULL)
			appendPQExpBuffer(query, " '%s'", options[(i * 2) + 1]);
	}

	if (noptions)
		appendPQExpBufferChar(query, ')');

	res = PQexec(conn, query->data);
	if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		pg_log_error("could not send replication command \"%s\": %s",
					 query->data, PQresultErrorMessage(res));
		PQclear(res);
		goto error;
	}
	PQclear(res);
	resetPQExpBuffer(query);

	if (verbose)
		pg_log_info("streaming initiated");

	while (!time_to_abort)
	{
		int			r;
		int			bytes_left;
		int			bytes_written;
		TimestampTz now;
		int			hdr_len;
		XLogRecPtr	cur_record_lsn = InvalidXLogRecPtr;

		if (copybuf != NULL)
		{
			PQfreemem(copybuf);
			copybuf = NULL;
		}

		/*
		 * 可能向主服务器发送一条状态消息。 
		 */
		now = feGetCurrentTimestamp();

		if (outfd != -1 &&
			feTimestampDifferenceExceeds(output_last_fsync, now,
										 fsync_interval))
		{
			if (!OutputFsync(now))
				goto error;
		}

		if (standby_message_timeout > 0 &&
			feTimestampDifferenceExceeds(last_status, now,
										 standby_message_timeout))
		{
			/* 该发反馈！        */
			if (!sendFeedback(conn, now, true, false))
				goto error;

			last_status = now;
		}

		/* SIGHUP，关输出文件      */
		if (outfd != -1 && output_reopen && strcmp(outfile, "-") != 0)
		{
			now = feGetCurrentTimestamp();
			if (!OutputFsync(now))
				goto error;
			close(outfd);
			outfd = -1;
		}
		output_reopen = false;

		/* 输出文件未开则开              */
		if (outfd == -1)
		{
			struct stat statbuf;

			if (strcmp(outfile, "-") == 0)
				outfd = fileno(stdout);
			else
				outfd = open(outfile, O_CREAT | O_APPEND | O_WRONLY | PG_BINARY,
							 S_IRUSR | S_IWUSR);
			if (outfd == -1)
			{
				pg_log_error("could not open log file \"%s\": %m", outfile);
				goto error;
			}

			if (fstat(outfd, &statbuf) != 0)
			{
				pg_log_error("could not stat file \"%s\": %m", outfile);
				goto error;
			}

			output_isfile = S_ISREG(statbuf.st_mode) && !isatty(outfd);
		}

		r = PQgetCopyData(conn, &copybuf, 1);
		if (r == 0)
		{
			/*
			 * 处于异步模式且没有可用数据。我们会阻塞在 
			 * 读取上但不超过指定的超时，以便向客户    
			 * 端发回响应。          
			 */
			fd_set		input_mask;
			TimestampTz message_target = 0;
			TimestampTz fsync_target = 0;
			struct timeval timeout;
			struct timeval *timeoutptr = NULL;

			if (PQsocket(conn) < 0)
			{
				pg_log_error("invalid socket: %s", PQerrorMessage(conn));
				goto error;
			}

			FD_ZERO(&input_mask);
			FD_SET(PQsocket(conn), &input_mask);

			/* 算醒来发 keepalive 时间                               */
			if (standby_message_timeout)
				message_target = last_status + (standby_message_timeout - 1) *
					((int64) 1000);

			/* 算醒来 fsync 输出时间                             */
			if (fsync_interval > 0 && output_needs_fsync)
				fsync_target = output_last_fsync + (fsync_interval - 1) *
					((int64) 1000);

			/* 算醒来时间             */
			if (message_target > 0 || fsync_target > 0)
			{
				TimestampTz targettime;
				long		secs;
				int			usecs;

				targettime = message_target;

				if (fsync_target > 0 && fsync_target < targettime)
					targettime = fsync_target;

				feTimestampDifference(now,
									  targettime,
									  &secs,
									  &usecs);
				if (secs <= 0)
					timeout.tv_sec = 1; /* 至少休眠 1 秒          */
				else
					timeout.tv_sec = secs;
				timeout.tv_usec = usecs;
				timeoutptr = &timeout;
			}

			r = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, timeoutptr);
			if (r == 0 || (r < 0 && errno == EINTR))
			{
				/*
				 * 超时或收到信号。继续循环，要么发送  
				 * 状态包给服务器，要么直接回到阻塞状态。 
				 *          
				 */
				continue;
			}
			else if (r < 0)
			{
				pg_log_error("%s() failed: %m", "select");
				goto error;
			}

			/* 套接字确有数据                     */
			if (PQconsumeInput(conn) == 0)
			{
				pg_log_error("could not receive data from WAL stream: %s",
							 PQerrorMessage(conn));
				goto error;
			}
			continue;
		}

		/* copy 流结束     */
		if (r == -1)
			break;

		/* 读 copy 流失败                    */
		if (r == -2)
		{
			pg_log_error("could not read COPY data: %s",
						 PQerrorMessage(conn));
			goto error;
		}

		/* 检查消息类型      */
		if (copybuf[0] == 'k')
		{
			int			pos;
			bool		replyRequested;
			XLogRecPtr	walEnd;
			bool		endposReached = false;

			/*
			 * 解析 CopyData 消息内包含的 keepalive 消息。我们  
			 * 只检查服务器是否请求了回复，其余内容忽略 
			 *      
			 */
			pos = 1;			/* 跳 msgtype k    */
			walEnd = fe_recvint64(&copybuf[pos]);
			output_written_lsn = Max(walEnd, output_written_lsn);

			pos += 8;			/* 读 walEnd  */

			pos += 8;			/* 跳 sendTime  */

			if (r < pos + 1)
			{
				pg_log_error("streaming header too small: %d", r);
				goto error;
			}
			replyRequested = copybuf[pos];

			if (endpos != InvalidXLogRecPtr && walEnd >= endpos)
			{
				/*
				 * 若在 keepalive 之前套接字上无可读内容，     
				 * 我们就知道服务器已没有要发送的数据；
				 * 而如果 walEnd 已经超过 endpos，我们就知道 
				 * 在 endpos 前无其他提交，即可退出。  
				 */
				endposReached = true;
			}

			/* 必要回复               */
			if (replyRequested || endposReached)
			{
				if (!flushAndSendFeedback(conn, &now))
					goto error;
				last_status = now;
			}

			if (endposReached)
			{
				prepareToTerminate(conn, endpos, true, InvalidXLogRecPtr);
				time_to_abort = true;
				break;
			}

			continue;
		}
		else if (copybuf[0] != 'w')
		{
			pg_log_error("unrecognized streaming header: \"%c\"",
						 copybuf[0]);
			goto error;
		}

		/*
		 * 读取 CopyData 消息内包含的 XLogData 消息的头部。   
		 * 我们只需要 WAL 位置字段 (dataStart)，头部其余部分 
		 * 都被忽略。          
		 */
		hdr_len = 1;			/* 类型 'w'  */
		hdr_len += 8;			/* dataStart */
		hdr_len += 8;			/* walEnd */
		hdr_len += 8;			/* 发时   */
		if (r < hdr_len + 1)
		{
			pg_log_error("streaming header too small: %d", r);
			goto error;
		}

		/* 提取此块的 WAL 位置          */
		cur_record_lsn = fe_recvint64(&copybuf[1]);

		if (endpos != InvalidXLogRecPtr && cur_record_lsn > endpos)
		{
			/*
			 * 我们已经读过了终点，接下来准备退出，并
			 * 谨慎处理输出数据的去向。           
			 */
			if (!flushAndSendFeedback(conn, &now))
				goto error;
			prepareToTerminate(conn, endpos, false, cur_record_lsn);
			time_to_abort = true;
			break;
		}

		output_written_lsn = Max(cur_record_lsn, output_written_lsn);

		bytes_left = r - hdr_len;
		bytes_written = 0;

		/* 标记需 fsync               */
		output_needs_fsync = true;

		while (bytes_left)
		{
			int			ret;

			ret = write(outfd,
						copybuf + hdr_len + bytes_written,
						bytes_left);

			if (ret < 0)
			{
				pg_log_error("could not write %d bytes to log file \"%s\": %m",
							 bytes_left, outfile);
				goto error;
			}

			/* 写入成功，推进我们的位置       */
			bytes_written += ret;
			bytes_left -= ret;
		}

		if (write(outfd, "\n", 1) != 1)
		{
			pg_log_error("could not write %d bytes to log file \"%s\": %m",
						 1, outfile);
			goto error;
		}

		if (endpos != InvalidXLogRecPtr && cur_record_lsn == endpos)
		{
			/* endpos 刚处理记录，完成                             */
			if (!flushAndSendFeedback(conn, &now))
				goto error;
			prepareToTerminate(conn, endpos, false, cur_record_lsn);
			time_to_abort = true;
			break;
		}
	}

	res = PQgetResult(conn);
	if (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		PQclear(res);

		/*
		 * 我们在执行客户端发起的干净退出，且已把 CopyDone 
		 * 发送给了服务器。要排空全部消息，以免漏掉  
		 *  ErrorResponse。walsender 一旦看到 CopyDone，就             
		 * 停止生成 XLogData 记录，预计很快就会结束。         
		 * 在 CopyDone 之后就无法再调用 sendFeedback() 了，即使 
		 * 耗时再长，也要用同步的 PQgetCopyData() 
		 */
		while (1)
		{
			int			r;

			if (copybuf != NULL)
			{
				PQfreemem(copybuf);
				copybuf = NULL;
			}
			r = PQgetCopyData(conn, &copybuf, 0);
			if (r == -1)
				break;
			if (r == -2)
			{
				pg_log_error("could not read COPY data: %s",
							 PQerrorMessage(conn));
				time_to_abort = false;	/* 异常退出 */
				goto error;
			}
		}

		res = PQgetResult(conn);
	}
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("unexpected termination of replication stream: %s",
					 PQresultErrorMessage(res));
		goto error;
	}
	PQclear(res);

	if (outfd != -1 && strcmp(outfile, "-") != 0)
	{
		TimestampTz t = feGetCurrentTimestamp();

		/* 此处失败无需跳到错误处理，反正即将结束        */
		OutputFsync(t);

		if (close(outfd) != 0)
			pg_log_error("could not close file \"%s\": %m", outfile);
	}
	outfd = -1;
error:
	if (copybuf != NULL)
	{
		PQfreemem(copybuf);
		copybuf = NULL;
	}
	destroyPQExpBuffer(query);
	PQfinish(conn);
	conn = NULL;
}

/*
 * 遗憾的是，在 Windows 上无法合理地处理信号…… 
 */
#ifndef WIN32

/*
 * 当捕获到 SIGINT/SIGTERM 时，只是让系统在下一个可能的  
 * 时刻退出。 
 */
static void
sigexit_handler(SIGNAL_ARGS)
{
	time_to_abort = true;
}

/*
 * 触发输出文件重新打开。      
 */
static void
sighup_handler(SIGNAL_ARGS)
{
	output_reopen = true;
}
#endif


int
main(int argc, char **argv)
{
	static struct option long_options[] = {
/* 通用选项    */
		{"file", required_argument, NULL, 'f'},
		{"fsync-interval", required_argument, NULL, 'F'},
		{"no-loop", no_argument, NULL, 'n'},
		{"verbose", no_argument, NULL, 'v'},
		{"two-phase", no_argument, NULL, 't'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, '?'},
/* 连接选项       */
		{"dbname", required_argument, NULL, 'd'},
		{"host", required_argument, NULL, 'h'},
		{"port", required_argument, NULL, 'p'},
		{"username", required_argument, NULL, 'U'},
		{"no-password", no_argument, NULL, 'w'},
		{"password", no_argument, NULL, 'W'},
/* 复制选项        */
		{"startpos", required_argument, NULL, 'I'},
		{"endpos", required_argument, NULL, 'E'},
		{"option", required_argument, NULL, 'o'},
		{"plugin", required_argument, NULL, 'P'},
		{"status-interval", required_argument, NULL, 's'},
		{"slot", required_argument, NULL, 'S'},
/* 操作 */
		{"create-slot", no_argument, NULL, 1},
		{"start", no_argument, NULL, 2},
		{"drop-slot", no_argument, NULL, 3},
		{"if-not-exists", no_argument, NULL, 4},
		{NULL, 0, NULL, 0}
	};
	int			c;
	int			option_index;
	uint32		hi,
				lo;
	char	   *db_name;

	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_basebackup"));

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		else if (strcmp(argv[1], "-V") == 0 ||
				 strcmp(argv[1], "--version") == 0)
		{
			puts("pg_recvlogical (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "E:f:F:ntvd:h:p:U:wWI:o:P:s:S:",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
/* 通用选项    */
			case 'f':
				outfile = pg_strdup(optarg);
				break;
			case 'F':
				if (!option_parse_int(optarg, "-F/--fsync-interval", 0,
									  INT_MAX / 1000,
									  &fsync_interval))
					exit(1);
				fsync_interval *= 1000;
				break;
			case 'n':
				noloop = 1;
				break;
			case 't':
				two_phase = true;
				break;
			case 'v':
				verbose++;
				break;
/* 连接选项       */
			case 'd':
				dbname = pg_strdup(optarg);
				break;
			case 'h':
				dbhost = pg_strdup(optarg);
				break;
			case 'p':
				dbport = pg_strdup(optarg);
				break;
			case 'U':
				dbuser = pg_strdup(optarg);
				break;
			case 'w':
				dbgetpassword = -1;
				break;
			case 'W':
				dbgetpassword = 1;
				break;
/* 复制选项        */
			case 'I':
				if (sscanf(optarg, "%X/%X", &hi, &lo) != 2)
					pg_fatal("could not parse start position \"%s\"", optarg);
				startpos = ((uint64) hi) << 32 | lo;
				break;
			case 'E':
				if (sscanf(optarg, "%X/%X", &hi, &lo) != 2)
					pg_fatal("could not parse end position \"%s\"", optarg);
				endpos = ((uint64) hi) << 32 | lo;
				break;
			case 'o':
				{
					char	   *data = pg_strdup(optarg);
					char	   *val = strchr(data, '=');

					if (val != NULL)
					{
						/* 去=分数据与值              */
						*val = '\0';
						val++;
					}

					noptions += 1;
					options = pg_realloc(options, sizeof(char *) * noptions * 2);

					options[(noptions - 1) * 2] = data;
					options[(noptions - 1) * 2 + 1] = val;
				}

				break;
			case 'P':
				plugin = pg_strdup(optarg);
				break;
			case 's':
				if (!option_parse_int(optarg, "-s/--status-interval", 0,
									  INT_MAX / 1000,
									  &standby_message_timeout))
					exit(1);
				standby_message_timeout *= 1000;
				break;
			case 'S':
				replication_slot = pg_strdup(optarg);
				break;
/* 操作 */
			case 1:
				do_create_slot = true;
				break;
			case 2:
				do_start_slot = true;
				break;
			case 3:
				do_drop_slot = true;
				break;
			case 4:
				slot_exists_ok = true;
				break;

			default:
				/* getopt_long 已投诉                   */
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}

	/*
	 * 有非选项参数吗？ 
	 */
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")",
					 argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 必需的参数   
	 */
	if (replication_slot == NULL)
	{
		pg_log_error("no slot specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (do_start_slot && outfile == NULL)
	{
		pg_log_error("no target file specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!do_drop_slot && dbname == NULL)
	{
		pg_log_error("no database specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (!do_drop_slot && !do_create_slot && !do_start_slot)
	{
		pg_log_error("at least one action needs to be specified");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (do_drop_slot && (do_create_slot || do_start_slot))
	{
		pg_log_error("cannot use --create-slot or --start together with --drop-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (startpos != InvalidXLogRecPtr && (do_create_slot || do_drop_slot))
	{
		pg_log_error("cannot use --create-slot or --drop-slot together with --startpos");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (endpos != InvalidXLogRecPtr && !do_start_slot)
	{
		pg_log_error("--endpos may only be specified with --start");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	if (two_phase && !do_create_slot)
	{
		pg_log_error("--two-phase may only be specified with --create-slot");
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	/*
	 * 获得服务器的连接。尤其如果需要密码，我们想立即  
	 * 从用户那里收集密码。          
	 */
	conn = GetConnection();
	if (!conn)
		/* 错误已在 GetConnection 写入                */
		exit(1);
	atexit(disconnect_atexit);

	/*
	 * 捕获信号。（如果 GetConnection 中需要密码提示，       
	 * 则不要在此之前执行。）   
	 */
#ifndef WIN32
	pqsignal(SIGINT, sigexit_handler);
	pqsignal(SIGTERM, sigexit_handler);
	pqsignal(SIGHUP, sighup_handler);
#endif

	/*
	 * 运行 IDENTIFY_SYSTEM 以确认我们用的是数据库特定的复   
	 * 制连接。           
	 */
	if (!RunIdentifySystem(conn, NULL, NULL, NULL, &db_name))
		exit(1);

	if (db_name == NULL)
		pg_fatal("could not establish database-specific replication connection");

	/*
	 * 设置 umask，使新建的目录/文件与源数据目录中
	 * 的目录/文件具有相同的权限。                      
	 *
	 * pg_mode_mask 默认设为仅属主，随后在                  
	 * GetConnection() 中更新，通过 RetrieveDataDirCreatePerm() 
	 * 从服务器端获取该模式并调用 SetDataDirectoryCreatePerm()。
	 */
	umask(pg_mode_mask);

	/* 删除复制槽          */
	if (do_drop_slot)
	{
		if (verbose)
			pg_log_info("dropping replication slot \"%s\"", replication_slot);

		if (!DropReplicationSlot(conn, replication_slot))
			exit(1);
	}

	/* 创建复制槽            */
	if (do_create_slot)
	{
		if (verbose)
			pg_log_info("creating replication slot \"%s\"", replication_slot);

		if (!CreateReplicationSlot(conn, replication_slot, plugin, false,
								   false, false, slot_exists_ok, two_phase))
			exit(1);
		startpos = InvalidXLogRecPtr;
	}

	if (!do_start_slot)
		exit(0);

	/* 流循环   */
	while (true)
	{
		StreamLogicalLog();
		if (time_to_abort)
		{
			/*
			 * 我们已被 Ctrl-C 中断或达到了退出限制条件。这 
			 * 并不是错误，不带错误码退出。 
			 */
			exit(0);
		}
		else if (noloop)
			pg_fatal("disconnected");
		else
		{
			/* translator: check source for value for %d */
			pg_log_info("disconnected; waiting %d seconds to try again",
						RECONNECT_SLEEP_TIME);
			pg_usleep(RECONNECT_SLEEP_TIME * 1000000);
		}
	}
}

/*
 * fsync 输出数据，并向服务器发送反馈消息。成功时返回  
 * true，否则返回 false。        
 *
 * 成功时，*now 会在发送反馈之前被更新为当前的时间戳。 
 *          
 */
static bool
flushAndSendFeedback(PGconn *conn, TimestampTz *now)
{
	/* 刷新数据到盘发最新刷新指针                    */
	if (!OutputFsync(*now))
		return false;
	*now = feGetCurrentTimestamp();
	if (!sendFeedback(conn, *now, true, false))
		return false;

	return true;
}

/*
 * 尝试告知服务器我们即将终止，但不要等待，也不要在    
 * 失败时重试  
 */
static void
prepareToTerminate(PGconn *conn, XLogRecPtr endpos, bool keepalive, XLogRecPtr lsn)
{
	(void) PQputCopyEnd(conn, NULL);
	(void) PQflush(conn);

	if (verbose)
	{
		if (keepalive)
			pg_log_info("end position %X/%X reached by keepalive",
						LSN_FORMAT_ARGS(endpos));
		else
			pg_log_info("end position %X/%X reached by WAL record at %X/%X",
						LSN_FORMAT_ARGS(endpos), LSN_FORMAT_ARGS(lsn));
	}
}
