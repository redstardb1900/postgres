/*-------------------------------------------------------------------------
 *
 * libpq_source.c
 *	  Functions for fetching files from a remote server via libpq.
 *
 * Copyright (c) 2013-2023, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "catalog/pg_type_d.h"
#include "common/connect.h"
#include "datapagemap.h"
#include "file_ops.h"
#include "filemap.h"
#include "lib/stringinfo.h"
#include "pg_rewind.h"
#include "port/pg_bswap.h"
#include "rewind_source.h"

/*
 * 文件每次获取 MAX_CHUNK_SIZE 字节，单次查询最多
 * MAX_CHUNKS_PER_QUERY 个块。
                                                               */
#define MAX_CHUNK_SIZE (1024 * 1024)
#define MAX_CHUNKS_PER_QUERY 1000

/* 表示从源获取文件片段的请求                                                   */
typedef struct
{
	const char *path;			/* 相对数据目录根的路径       */
	off_t		offset;
	size_t		length;
} fetch_range_request;

typedef struct
{
	rewind_source common;		/* common interface functions */

	PGconn	   *conn;

	/*
	 * 通过 queue_fetch_range() 请求、尚未从远程获取的块队列。
	 *                                 
	                             */
	int			num_requests;
	fetch_range_request request_queue[MAX_CHUNKS_PER_QUERY];

	/* process_queued_fetch_requests() 的临时空间               */
	StringInfoData paths;
	StringInfoData offsets;
	StringInfoData lengths;
} libpq_source;

static void init_libpq_conn(PGconn *conn);
static char *run_simple_query(PGconn *conn, const char *sql);
static void run_simple_command(PGconn *conn, const char *sql);
static void appendArrayEscapedString(StringInfo buf, const char *str);

static void process_queued_fetch_requests(libpq_source *src);

/* public interface functions */
static void libpq_traverse_files(rewind_source *source,
								 process_file_callback_t callback);
static void libpq_queue_fetch_file(rewind_source *source, const char *path, size_t len);
static void libpq_queue_fetch_range(rewind_source *source, const char *path,
									off_t off, size_t len);
static void libpq_finish_fetch(rewind_source *source);
static char *libpq_fetch_file(rewind_source *source, const char *path,
							  size_t *filesize);
static XLogRecPtr libpq_get_current_wal_insert_lsn(rewind_source *source);
static void libpq_destroy(rewind_source *source);

/*
 * 创建新的 libpq 源。
 *
 * 调用者已建立连接，但在源处于活动状态时不应
 * 尝试使用它。
                                                                                             */
rewind_source *
init_libpq_source(PGconn *conn)
{
	libpq_source *src;

	init_libpq_conn(conn);

	src = pg_malloc0(sizeof(libpq_source));

	src->common.traverse_files = libpq_traverse_files;
	src->common.fetch_file = libpq_fetch_file;
	src->common.queue_fetch_file = libpq_queue_fetch_file;
	src->common.queue_fetch_range = libpq_queue_fetch_range;
	src->common.finish_fetch = libpq_finish_fetch;
	src->common.get_current_wal_insert_lsn = libpq_get_current_wal_insert_lsn;
	src->common.destroy = libpq_destroy;

	src->conn = conn;

	initStringInfo(&src->paths);
	initStringInfo(&src->offsets);
	initStringInfo(&src->lengths);

	return &src->common;
}

/*
 * 初始化 libpq 连接以供使用。
                      */
static void
init_libpq_conn(PGconn *conn)
{
	PGresult   *res;
	char	   *str;

	/* 禁用所有类型的超时                     */
	run_simple_command(conn, "SET statement_timeout = 0");
	run_simple_command(conn, "SET lock_timeout = 0");
	run_simple_command(conn, "SET idle_in_transaction_session_timeout = 0");

	/*
	 * 无更新意图，将连接置于只读模式以确保该约束。
	 *        
	                */
	run_simple_command(conn, "SET default_transaction_read_only = on");

	/* 安全 search_path     */
	res = PQexec(conn, ALWAYS_SECURE_SEARCH_PATH_SQL);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not clear search_path: %s",
				 PQresultErrorMessage(res));
	PQclear(res);

	/*
	 * 同样检查 full_page_writes 是否启用。通过 pg_read_binary_file()
	 * 读页时若被修改会产生 torn page，依赖全页镜像修复。
	 *                     
	               */
	str = run_simple_query(conn, "SHOW full_page_writes");
	if (strcmp(str, "on") != 0)
		pg_fatal("full_page_writes must be enabled in the source server");
	pg_free(str);

	/* 准备用于获取文件的语句                                  */
	res = PQprepare(conn, "fetch_chunks_stmt",
					"SELECT path, begin,\n"
					"  pg_read_binary_file(path, begin, len, true) AS chunk\n"
					"FROM unnest ($1::text[], $2::int8[], $3::int4[]) as x(path, begin, len)",
					3, NULL);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("could not prepare statement to fetch file contents: %s",
				 PQresultErrorMessage(res));
	PQclear(res);
}

/*
 * 运行返回单个值的查询。
 *
 * 结果在使用后应用 pg_free 释放。
                                                   */
static char *
run_simple_query(PGconn *conn, const char *sql)
{
	PGresult   *res;
	char	   *result;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("error running query (%s) on source server: %s",
				 sql, PQresultErrorMessage(res));

	/* 健全性检查结果集                    */
	if (PQnfields(res) != 1 || PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("unexpected result set from query");

	result = pg_strdup(PQgetvalue(res, 0, 0));

	PQclear(res);

	return result;
}

/*
 * 运行命令。
 *
 * 若发生失败，立即退出。
                                           */
static void
run_simple_command(PGconn *conn, const char *sql)
{
	PGresult   *res;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pg_fatal("error running query (%s) in source server: %s",
				 sql, PQresultErrorMessage(res));

	PQclear(res);
}

/*
 * 调用远程系统中的 pg_current_wal_insert_lsn()
 * 函数。
                         */
static XLogRecPtr
libpq_get_current_wal_insert_lsn(rewind_source *source)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
	XLogRecPtr	result;
	uint32		hi;
	uint32		lo;
	char	   *val;

	val = run_simple_query(conn, "SELECT pg_current_wal_insert_lsn()");

	if (sscanf(val, "%X/%X", &hi, &lo) != 2)
		pg_fatal("unrecognized result \"%s\" for current WAL insert location", val);

	result = ((uint64) hi) << 32 | lo;

	pg_free(val);

	return result;
}

/*
 * 获取数据目录中所有文件的列表。
                                */
static void
libpq_traverse_files(rewind_source *source, process_file_callback_t callback)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
	PGresult   *res;
	const char *sql;
	int			i;

	/*
	 * 创建整个数据目录的递归文件列表。
	 * 
	 * WITH RECURSIVE 部分完成主要工作。第二部分获取 pg_tblspc 目录中的
	 * 符号链接目标。
	 * 
	 * XXX: 尚无后端函数可通用获取符号链接目标地址，若管理员在数据目录
	 * 中放置自定义符号链接，将无法正确复制。
	 *                    
	                                          */
	sql =
		"WITH RECURSIVE files (path, filename, size, isdir) AS (\n"
		"  SELECT '' AS path, filename, size, isdir FROM\n"
		"  (SELECT pg_ls_dir('.', true, false) AS filename) AS fn,\n"
		"        pg_stat_file(fn.filename, true) AS this\n"
		"  UNION ALL\n"
		"  SELECT parent.path || parent.filename || '/' AS path,\n"
		"         fn, this.size, this.isdir\n"
		"  FROM files AS parent,\n"
		"       pg_ls_dir(parent.path || parent.filename, true, false) AS fn,\n"
		"       pg_stat_file(parent.path || parent.filename || '/' || fn, true) AS this\n"
		"       WHERE parent.isdir = 't'\n"
		")\n"
		"SELECT path || filename, size, isdir,\n"
		"       pg_tablespace_location(pg_tablespace.oid) AS link_target\n"
		"FROM files\n"
		"LEFT OUTER JOIN pg_tablespace ON files.path = 'pg_tblspc/'\n"
		"                             AND oid::text = files.filename\n";
	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not fetch file list: %s",
				 PQresultErrorMessage(res));

	/* 健全性检查结果集                    */
	if (PQnfields(res) != 4)
		pg_fatal("unexpected result set while fetching file list");

	/* 将结果读取到局部变量                     */
	for (i = 0; i < PQntuples(res); i++)
	{
		char	   *path;
		int64		filesize;
		bool		isdir;
		char	   *link_target;
		file_type_t type;

		if (PQgetisnull(res, i, 1))
		{
			/*
			 * 查询运行期间文件从服务器删除，忽略。
			 *         
			              */
			continue;
		}

		path = PQgetvalue(res, i, 0);
		filesize = atol(PQgetvalue(res, i, 1));
		isdir = (strcmp(PQgetvalue(res, i, 2), "t") == 0);
		link_target = PQgetvalue(res, i, 3);

		if (link_target[0])
			type = FILE_TYPE_SYMLINK;
		else if (isdir)
			type = FILE_TYPE_DIRECTORY;
		else
			type = FILE_TYPE_REGULAR;

		callback(path, type, filesize, link_target);
	}
	PQclear(res);
}

/*
 * 排队请求从远程系统获取文件。
                                         */
static void
libpq_queue_fetch_file(rewind_source *source, const char *path, size_t len)
{
	/*
	 * 立即截断目标文件，排队从源获取。若文件小于 MAX_CHUNK_SIZE，
	 * 仍请求完整块，以便扫描源目录后若文件变大，仍能获取完整
	 * 文件。此机制仅对不超过 MAX_CHUNK_SIZE 的文件有效，但对于
	 * 偶尔更改的小配置文件等（非 WAL 日志记录的）已足够。对于
	 * 较大文件，最多获取到原始大小。
	 * 
	 * 即便如此仍存在固有竞态：若复制的同时文件被修改，可能得到
	 * 撕裂版本（部分旧、部分新）。但 pg_basebackup 同样有此问题，
	 * 实践中并未成为麻烦。
	 * 
	 * 收到文件后再截断看似更自然，但那样就需要跟踪哪些获取请求
	 * 针对的是完整文件。
	 * 
	 * 
	 * 
	 * 
	 * 
	 * 
	                                                                                                                                                                                                                                                                                                                                                    */
	open_target_file(path, true);
	libpq_queue_fetch_range(source, path, 0, Max(len, MAX_CHUNK_SIZE));
}

/*
 * 排队请求从远程系统获取文件片段。
                                                  */
static void
libpq_queue_fetch_range(rewind_source *source, const char *path, off_t off,
						size_t len)
{
	libpq_source *src = (libpq_source *) source;

	/*
	 * 此请求是否恰为前一个块的延续？若是则合并。
	 *       
	 * 
	 * XXX: 通过指针相等比较路径，对我们的场景已足够；
	 * 调用者总对同一文件传相同指针，否则合并失败而不影响正确性。
	 * 
	 * 
	                                                                                                                 */
	if (src->num_requests > 0)
	{
		fetch_range_request *prev = &src->request_queue[src->num_requests - 1];

		if (prev->offset + prev->length == off &&
			prev->length < MAX_CHUNK_SIZE &&
			prev->path == path)
		{
			/*
			 * 扩展前一请求以尽量覆盖此新请求，不超出 MAX_CHUNK_SIZE。
			 *                        
			             */
			size_t		thislen;

			thislen = Min(len, MAX_CHUNK_SIZE - prev->length);
			prev->length += thislen;

			off += thislen;
			len -= thislen;

			/*
			 * Fall through to create new requests for any remaining 'len'
			 * that didn't fit in the previous chunk.
			 */
		}
	}

	/* 将请求分解为每个 MAX_CHUNK_SIZE 字节的片段                               */
	while (len > 0)
	{
		int32		thislen;

		/* 若队列已满，执行目前排队的所有工作                                           */
		if (src->num_requests == MAX_CHUNKS_PER_QUERY)
			process_queued_fetch_requests(src);

		thislen = Min(len, MAX_CHUNK_SIZE);
		src->request_queue[src->num_requests].path = path;
		src->request_queue[src->num_requests].offset = off;
		src->request_queue[src->num_requests].length = thislen;
		src->num_requests++;

		off += thislen;
		len -= thislen;
	}
}

/*
 * 获取所有排队的块并将其写入目标数据目录。
                                                     */
static void
libpq_finish_fetch(rewind_source *source)
{
	process_queued_fetch_requests((libpq_source *) source);
}

static void
process_queued_fetch_requests(libpq_source *src)
{
	const char *params[3];
	PGresult   *res;
	int			chunkno;

	if (src->num_requests == 0)
		return;

	pg_log_debug("getting %d file chunks", src->num_requests);

	/*
	 * 预编译语句 fetch_chunks_stmt 以三个等长数组为参数：
	 * paths、offsets 与 lengths。构造其字符串表示。
	 *      
	                                           */
	resetStringInfo(&src->paths);
	resetStringInfo(&src->offsets);
	resetStringInfo(&src->lengths);

	appendStringInfoChar(&src->paths, '{');
	appendStringInfoChar(&src->offsets, '{');
	appendStringInfoChar(&src->lengths, '{');
	for (int i = 0; i < src->num_requests; i++)
	{
		fetch_range_request *rq = &src->request_queue[i];

		if (i > 0)
		{
			appendStringInfoChar(&src->paths, ',');
			appendStringInfoChar(&src->offsets, ',');
			appendStringInfoChar(&src->lengths, ',');
		}

		appendArrayEscapedString(&src->paths, rq->path);
		appendStringInfo(&src->offsets, INT64_FORMAT, (int64) rq->offset);
		appendStringInfo(&src->lengths, INT64_FORMAT, (int64) rq->length);
	}
	appendStringInfoChar(&src->paths, '}');
	appendStringInfoChar(&src->offsets, '}');
	appendStringInfoChar(&src->lengths, '}');

	/*
	 * Execute the prepared statement.
	 */
	params[0] = src->paths.data;
	params[1] = src->offsets.data;
	params[2] = src->lengths.data;

	if (PQsendQueryPrepared(src->conn, "fetch_chunks_stmt", 3, params, NULL, NULL, 1) != 1)
		pg_fatal("could not send query: %s", PQerrorMessage(src->conn));

	if (PQsetSingleRowMode(src->conn) != 1)
		pg_fatal("could not set libpq connection to single row mode");

	/*----
	 * 结果集格式：
	 *
	 * path		text	-- 数据目录中的路径，如 "base/1/123"
	 * begin	int8	-- 文件内的偏移量
	 * chunk	bytea	-- 文件内容
	 *----
	                                                                   */
	chunkno = 0;
	while ((res = PQgetResult(src->conn)) != NULL)
	{
		fetch_range_request *rq = &src->request_queue[chunkno];
		char	   *filename;
		int			filenamelen;
		int64		chunkoff;
		int			chunksize;
		char	   *chunk;

		switch (PQresultStatus(res))
		{
			case PGRES_SINGLE_TUPLE:
				break;

			case PGRES_TUPLES_OK:
				PQclear(res);
				continue;		/* final zero-row result */

			default:
				pg_fatal("unexpected result while fetching remote files: %s",
						 PQresultErrorMessage(res));
		}

		if (chunkno > src->num_requests)
			pg_fatal("received more data chunks than requested");

		/* 健全性检查结果集                    */
		if (PQnfields(res) != 3 || PQntuples(res) != 1)
			pg_fatal("unexpected result set size while fetching remote files");

		if (PQftype(res, 0) != TEXTOID ||
			PQftype(res, 1) != INT8OID ||
			PQftype(res, 2) != BYTEAOID)
		{
			pg_fatal("unexpected data types in result set while fetching remote files: %u %u %u",
					 PQftype(res, 0), PQftype(res, 1), PQftype(res, 2));
		}

		if (PQfformat(res, 0) != 1 &&
			PQfformat(res, 1) != 1 &&
			PQfformat(res, 2) != 1)
		{
			pg_fatal("unexpected result format while fetching remote files");
		}

		if (PQgetisnull(res, 0, 0) ||
			PQgetisnull(res, 0, 1))
		{
			pg_fatal("unexpected null values in result while fetching remote files");
		}

		if (PQgetlength(res, 0, 1) != sizeof(int64))
			pg_fatal("unexpected result length while fetching remote files");

		/* 将结果集读取到局部变量                        */
		memcpy(&chunkoff, PQgetvalue(res, 0, 1), sizeof(int64));
		chunkoff = pg_ntoh64(chunkoff);
		chunksize = PQgetlength(res, 0, 2);

		filenamelen = PQgetlength(res, 0, 0);
		filename = pg_malloc(filenamelen + 1);
		memcpy(filename, PQgetvalue(res, 0, 0), filenamelen);
		filename[filenamelen] = '\0';

		chunk = PQgetvalue(res, 0, 2);

		/*
		 * 若文件在源端已删除，同样在目标端删除。同一文件可能有多个
		 * 数据块关联，可能多次 unlink()，因此无条件忽略任何缺失。
		 * 
		 * 
		                                                                    */
		if (PQgetisnull(res, 0, 2))
		{
			pg_log_debug("received null value for chunk for file \"%s\", file has been deleted",
						 filename);
			remove_target_file(filename, true);
		}
		else
		{
			pg_log_debug("received chunk for file \"%s\", offset %lld, size %d",
						 filename, (long long int) chunkoff, chunksize);

			if (strcmp(filename, rq->path) != 0)
			{
				pg_fatal("received data for file \"%s\", when requested for \"%s\"",
						 filename, rq->path);
			}
			if (chunkoff != rq->offset)
				pg_fatal("received data at offset %lld of file \"%s\", when requested for offset %lld",
						 (long long int) chunkoff, rq->path, (long long int) rq->offset);

			/*
			 * 不应接收超请求量的数据，否则 pg_read_binary_file() 出错。
			 * 但可能收到更少，若源端在检查大小后被截断。这无妨，应有
			 * WAL 记录记载该截断，pg_rewind 完成后首次启动目标时回放。
			 * 
			 * 
			 * 
			                                                                                                                   */
			if (chunksize > rq->length)
				pg_fatal("received more than requested for file \"%s\"", rq->path);

			open_target_file(filename, false);

			write_target_range(chunk, chunkoff, chunksize);
		}

		pg_free(filename);

		PQclear(res);
		chunkno++;
	}
	if (chunkno != src->num_requests)
		pg_fatal("unexpected number of data chunks received");

	src->num_requests = 0;
}

/*
 * 对字符串进行转义，用作文本数组常量中的元素
                                          */
static void
appendArrayEscapedString(StringInfo buf, const char *str)
{
	appendStringInfoCharMacro(buf, '\"');
	while (*str)
	{
		char		ch = *str;

		if (ch == '"' || ch == '\\')
			appendStringInfoCharMacro(buf, '\\');

		appendStringInfoCharMacro(buf, ch);

		str++;
	}
	appendStringInfoCharMacro(buf, '\"');
}

/*
 * 获取单个文件作为 malloc 分配的缓冲区。
                   */
static char *
libpq_fetch_file(rewind_source *source, const char *path, size_t *filesize)
{
	PGconn	   *conn = ((libpq_source *) source)->conn;
	PGresult   *res;
	char	   *result;
	int			len;
	const char *paramValues[1];

	paramValues[0] = path;
	res = PQexecParams(conn, "SELECT pg_read_binary_file($1)",
					   1, NULL, paramValues, NULL, NULL, 1);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pg_fatal("could not fetch remote file \"%s\": %s",
				 path, PQresultErrorMessage(res));

	/* 健全性检查结果集                    */
	if (PQntuples(res) != 1 || PQgetisnull(res, 0, 0))
		pg_fatal("unexpected result set while fetching remote file \"%s\"",
				 path);

	/* 将结果读取到局部变量                     */
	len = PQgetlength(res, 0, 0);
	result = pg_malloc(len + 1);
	memcpy(result, PQgetvalue(res, 0, 0), len);
	result[len] = '\0';

	PQclear(res);

	pg_log_debug("fetched file \"%s\", length %d", path, len);

	if (filesize)
		*filesize = len;
	return result;
}

/*
 * 关闭 libpq 源。
           */
static void
libpq_destroy(rewind_source *source)
{
	libpq_source *src = (libpq_source *) source;

	pfree(src->paths.data);
	pfree(src->offsets.data);
	pfree(src->lengths.data);
	pfree(src);

	/* 注意：此处不关闭连接，因为不是由我们打开的。                                                */
}
