/*-------------------------------------------------------------------------
 *
 * genfile.c
 *		直接访问文件的函数
 *
 *
 * Copyright (c) 2004-2023, PostgreSQL Global Development Group
 *
 * Author: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/genfile.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include "access/htup_details.h"
#include "access/xlog_internal.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/syslogger.h"
#include "replication/slot.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/*
 * 把 "text" 文件名参数转为 C 字符串，并检查是否允许。
 *
 * 文件名可为绝对路径，也可相对于 DataDir，但绝对
 * 路径必须匹配 DataDir 或 Log_directory。
 *
 * 本函数会对照 'pg_read_server_files' 角色做权限检查，
 * 因此它其实只适合只检查"读"权限的调用方使用。
 * 若你要找的是"写"或"program"访问的检查，请勿
 * 使用本函数，除非把它更新为把检查类型作为参数
 * 接受，并检查相应的角色成员资格。
 */
static char *
convert_and_check_filename(text *arg)
{
	char	   *filename;

	filename = text_to_cstring(arg);
	canonicalize_path(filename);	/* 这里文件名长度会变 */

	/*
	 * 拥有 'pg_read_server_files' 角色权限的角色允许
	 * 以 PG 用户身份访问服务器上的任何文件，
	 * 无须再检查。
	 */
	if (has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
		return filename;

	/*
	 * 用户非 pg_read_server_files 角色成员，故检查其是否
	 * 允许。
	 */
	if (is_absolute_path(filename))
	{
		/*
		 * 绝对路径位于 DataDir 或 Log_directory 内则允许，
		 * 即使 Log_directory 在 DataDir 之外。
		 */
		if (!path_is_prefix_of_path(DataDir, filename) &&
			(!is_absolute_path(Log_directory) ||
			 !path_is_prefix_of_path(Log_directory, filename)))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("absolute path not allowed")));
	}
	else if (!path_is_relative_and_below_cwd(filename))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("path must be in or below the data directory")));

	return filename;
}


/*
 * 读取文件的一段，以 bytea 返回
 *
 * 调用方负责全部权限检查。
 *
 * 当 bytes_to_read 为负时读取整个文件。
 */
static bytea *
read_binary_file(const char *filename, int64 seek_offset, int64 bytes_to_read,
				 bool missing_ok)
{
	bytea	   *buf;
	size_t		nbytes = 0;
	FILE	   *file;

	/* 把请求大小限制到实际能提供的量 */
	if (bytes_to_read > (int64) (MaxAllocSize - VARHDRSZ))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length too large")));

	if ((file = AllocateFile(filename, PG_BINARY_R)) == NULL)
	{
		if (missing_ok && errno == ENOENT)
			return NULL;
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" for reading: %m",
							filename)));
	}

	if (fseeko(file, (off_t) seek_offset,
			   (seek_offset >= 0) ? SEEK_SET : SEEK_END) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek in file \"%s\": %m", filename)));

	if (bytes_to_read >= 0)
	{
		/* 传入明确读取大小就直接读 */
		buf = (bytea *) palloc((Size) bytes_to_read + VARHDRSZ);

		nbytes = fread(VARDATA(buf), 1, (size_t) bytes_to_read, file);
	}
	else
	{
		/* 读取大小为负，读余下文件 */
		StringInfoData sbuf;

		initStringInfo(&sbuf);
		/* 在缓冲区中为 varlena 长度字留出空间 */
		sbuf.len += VARHDRSZ;
		Assert(sbuf.len < sbuf.maxlen);

		while (!(feof(file) || ferror(file)))
		{
			size_t		rbytes;

			/* 每次读取的最小量 */
#define MIN_READ_SIZE 4096

			/*
			 * 若未到文件尾且 sbuf.len 等于 MaxAllocSize -
			 * 1，那么要么文件过大，要么已无剩余内容。
			 * 尝试再读一个字节以确认是否已到文件尾。
			 * 若未到，则文件过大；我们宁可
			 * 由我们自己给出该错误消息。
			 */
			if (sbuf.len == MaxAllocSize - 1)
			{
				char		rbuf[1];

				if (fread(rbuf, 1, 1, file) != 0 || !feof(file))
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("file length too large")));
				else
					break;
			}

			/* 好，确保至少还能读取 MIN_READ_SIZE */
			enlargeStringInfo(&sbuf, MIN_READ_SIZE);

			/*
			 * stringinfo.c 喜欢按 2 的幂分配，因此很可能有
			 * 比所请求更多的空间可用。那就把它
			 * 全部用上，而不是多做不必要的 fread 调用。
			 */
			rbytes = fread(sbuf.data + sbuf.len, 1,
						   (size_t) (sbuf.maxlen - sbuf.len - 1), file);
			sbuf.len += rbytes;
			nbytes += rbytes;
		}

		/* 现在可以把 stringinfo 的缓冲区征用作结果 */
		buf = (bytea *) sbuf.data;
	}

	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));

	SET_VARSIZE(buf, nbytes + VARHDRSZ);

	FreeFile(file);

	return buf;
}

/*
 * 与 read_binary_file 类似，但会按数据库编码校验内容
 * 是否有效。
 */
static text *
read_text_file(const char *filename, int64 seek_offset, int64 bytes_to_read,
			   bool missing_ok)
{
	bytea	   *buf;

	buf = read_binary_file(filename, seek_offset, bytes_to_read, missing_ok);

	if (buf != NULL)
	{
		/* 确保输入有效 */
		pg_verifymbstr(VARDATA(buf), VARSIZE(buf) - VARHDRSZ, false);

		/* 好，可安全转为 text 了 */
		return (text *) buf;
	}
	else
		return NULL;
}

/*
 * 读取文件的一段，以 text 返回
 *
 * 保留此函数是为支持 adminpack 1.0。
 */
Datum
pg_read_file(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	int64		seek_offset = 0;
	int64		bytes_to_read = -1;
	bool		missing_ok = false;
	char	   *filename;
	text	   *result;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to read files with adminpack 1.0"),
		/* translator: %s is a SQL function name */
				 errhint("Consider using %s, which is part of core, instead.",
						 "pg_read_file()")));

	/* 处理可选参数 */
	if (PG_NARGS() >= 3)
	{
		seek_offset = PG_GETARG_INT64(1);
		bytes_to_read = PG_GETARG_INT64(2);

		if (bytes_to_read < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("requested length cannot be negative")));
	}
	if (PG_NARGS() >= 4)
		missing_ok = PG_GETARG_BOOL(3);

	filename = convert_and_check_filename(filename_t);

	result = read_text_file(filename, seek_offset, bytes_to_read, missing_ok);
	if (result)
		PG_RETURN_TEXT_P(result);
	else
		PG_RETURN_NULL();
}

/*
 * 读取文件的一段，以 text 返回
 *
 * 这里不做超级用户检查——权限处理交给 GRANT
 * 系统。
 *
 * 若 read_to_eof 为 true，则 bytes_to_read 必须是 -1，
 * 否则不可为负。
 */
static text *
pg_read_file_common(text *filename_t, int64 seek_offset, int64 bytes_to_read,
					bool read_to_eof, bool missing_ok)
{
	if (read_to_eof)
		Assert(bytes_to_read == -1);
	else if (bytes_to_read < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length cannot be negative")));

	return read_text_file(convert_and_check_filename(filename_t),
						  seek_offset, bytes_to_read, missing_ok);
}

/*
 * 读取文件的一段，以 bytea 返回
 *
 * 参数的解释方式与 pg_read_file_common() 相同。
 */
static bytea *
pg_read_binary_file_common(text *filename_t,
						   int64 seek_offset, int64 bytes_to_read,
						   bool read_to_eof, bool missing_ok)
{
	if (read_to_eof)
		Assert(bytes_to_read == -1);
	else if (bytes_to_read < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length cannot be negative")));

	return read_binary_file(convert_and_check_filename(filename_t),
							seek_offset, bytes_to_read, missing_ok);
}


/*
 * 下列 SQL 函数各变体的包装函数：pg_read_file() 与
 * pg_read_binary_file().
 *
 * 这些包装是为了通过 opr_sanity 的一致性检查所必需的，
 * 该检查会验证共享同一实现 C 函数的所有内建函数
 * 参数个数必须相同。
 */
Datum
pg_read_file_off_len(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	int64		seek_offset = PG_GETARG_INT64(1);
	int64		bytes_to_read = PG_GETARG_INT64(2);
	text	   *ret;

	ret = pg_read_file_common(filename_t, seek_offset, bytes_to_read,
							  false, false);
	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_read_file_off_len_missing(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	int64		seek_offset = PG_GETARG_INT64(1);
	int64		bytes_to_read = PG_GETARG_INT64(2);
	bool		missing_ok = PG_GETARG_BOOL(3);
	text	   *ret;

	ret = pg_read_file_common(filename_t, seek_offset, bytes_to_read,
							  false, missing_ok);

	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_read_file_all(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = pg_read_file_common(filename_t, 0, -1, true, false);

	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_read_file_all_missing(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	bool		missing_ok = PG_GETARG_BOOL(1);
	text	   *ret;

	ret = pg_read_file_common(filename_t, 0, -1, true, missing_ok);

	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(ret);
}

Datum
pg_read_binary_file_off_len(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	int64		seek_offset = PG_GETARG_INT64(1);
	int64		bytes_to_read = PG_GETARG_INT64(2);
	text	   *ret;

	ret = pg_read_binary_file_common(filename_t, seek_offset, bytes_to_read,
									 false, false);
	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(ret);
}

Datum
pg_read_binary_file_off_len_missing(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	int64		seek_offset = PG_GETARG_INT64(1);
	int64		bytes_to_read = PG_GETARG_INT64(2);
	bool		missing_ok = PG_GETARG_BOOL(3);
	text	   *ret;

	ret = pg_read_binary_file_common(filename_t, seek_offset, bytes_to_read,
									 false, missing_ok);
	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(ret);
}

Datum
pg_read_binary_file_all(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	text	   *ret;

	ret = pg_read_binary_file_common(filename_t, 0, -1, true, false);

	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(ret);
}

Datum
pg_read_binary_file_all_missing(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	bool		missing_ok = PG_GETARG_BOOL(1);
	text	   *ret;

	ret = pg_read_binary_file_common(filename_t, 0, -1, true, missing_ok);

	if (!ret)
		PG_RETURN_NULL();

	PG_RETURN_BYTEA_P(ret);
}

/*
 * stat 文件
 */
Datum
pg_stat_file(PG_FUNCTION_ARGS)
{
	text	   *filename_t = PG_GETARG_TEXT_PP(0);
	char	   *filename;
	struct stat fst;
	Datum		values[6];
	bool		isnull[6];
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	bool		missing_ok = false;

	/* 检查可选参数 */
	if (PG_NARGS() == 2)
		missing_ok = PG_GETARG_BOOL(1);

	filename = convert_and_check_filename(filename_t);

	if (stat(filename, &fst) < 0)
	{
		if (missing_ok && errno == ENOENT)
			PG_RETURN_NULL();
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", filename)));
	}

	/*
	 * 此记录类型最好与 pg_proc.h 中为我声明的输出参数
	 * 相匹配。
	 */
	tupdesc = CreateTemplateTupleDesc(6);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1,
					   "size", INT8OID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2,
					   "access", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3,
					   "modification", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4,
					   "change", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5,
					   "creation", TIMESTAMPTZOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6,
					   "isdir", BOOLOID, -1, 0);
	BlessTupleDesc(tupdesc);

	memset(isnull, false, sizeof(isnull));

	values[0] = Int64GetDatum((int64) fst.st_size);
	values[1] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_atime));
	values[2] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_mtime));
	/* Unix 是文件状态变更时间，而 Win32 是创建时间 */
#if !defined(WIN32) && !defined(__CYGWIN__)
	values[3] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_ctime));
	isnull[4] = true;
#else
	isnull[3] = true;
	values[4] = TimestampTzGetDatum(time_t_to_timestamptz(fst.st_ctime));
#endif
	values[5] = BoolGetDatum(S_ISDIR(fst.st_mode));

	tuple = heap_form_tuple(tupdesc, values, isnull);

	pfree(filename);

	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * stat 文件（1 参数版本）
 *
 * note: 此包装是通过 opr_sanity 一致性检查所必需的，
 * 该检查验证共享同一实现 C 函数的所有内建函数
 * 取相同个数的参数
 */
Datum
pg_stat_file_1arg(PG_FUNCTION_ARGS)
{
	return pg_stat_file(fcinfo);
}

/*
 * 列出目录（只返回文件名）
 */
Datum
pg_ls_dir(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location;
	bool		missing_ok = false;
	bool		include_dot_dirs = false;
	DIR		   *dirdesc;
	struct dirent *de;

	location = convert_and_check_filename(PG_GETARG_TEXT_PP(0));

	/* 检查可选参数 */
	if (PG_NARGS() == 3)
	{
		if (!PG_ARGISNULL(1))
			missing_ok = PG_GETARG_BOOL(1);
		if (!PG_ARGISNULL(2))
			include_dot_dirs = PG_GETARG_BOOL(2);
	}

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);

	dirdesc = AllocateDir(location);
	if (!dirdesc)
	{
		/* 合适时返回空的 tuplestore */
		if (missing_ok && errno == ENOENT)
			return (Datum) 0;
		/* 否则可以让 ReadDir() 抛出错误 */
	}

	while ((de = ReadDir(dirdesc, location)) != NULL)
	{
		Datum		values[1];
		bool		nulls[1];

		if (!include_dot_dirs &&
			(strcmp(de->d_name, ".") == 0 ||
			 strcmp(de->d_name, "..") == 0))
			continue;

		values[0] = CStringGetTextDatum(de->d_name);
		nulls[0] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc,
							 values, nulls);
	}

	FreeDir(dirdesc);
	return (Datum) 0;
}

/*
 * 列出目录（1 参数版本）
 *
 * note: 此包装是通过 opr_sanity 一致性检查所必需的，
 * 该检查验证共享同一实现 C 函数的所有内建函数
 * 取相同个数的参数。
 */
Datum
pg_ls_dir_1arg(PG_FUNCTION_ARGS)
{
	return pg_ls_dir(fcinfo);
}

/*
 * 返回目录文件列表的通用函数。
 *
 * 若目录不存在，在 missing_ok 为真时静默返回空集。
 * 其他不可读目录的情形会抛出错误。
 */
static Datum
pg_ls_dir_files(FunctionCallInfo fcinfo, const char *dir, bool missing_ok)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	DIR		   *dirdesc;
	struct dirent *de;

	InitMaterializedSRF(fcinfo, 0);

	/*
	 * 现在遍历目录。注意必须在单次 SRF 调用内
	 * 完成此事，不能让目录跨多次调用保持打开，因为
	 * 我们无法指望 SRF 一定会运行完毕。
	 */
	dirdesc = AllocateDir(dir);
	if (!dirdesc)
	{
		/* 合适时返回空的 tuplestore */
		if (missing_ok && errno == ENOENT)
			return (Datum) 0;
		/* 否则可以让 ReadDir() 抛出错误 */
	}

	while ((de = ReadDir(dirdesc, dir)) != NULL)
	{
		Datum		values[3];
		bool		nulls[3];
		char		path[MAXPGPATH * 2];
		struct stat attrib;

		/* 略隐藏文件 */
		if (de->d_name[0] == '.')
			continue;

		/* 取文件信息 */
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		if (stat(path, &attrib) < 0)
		{
			/* 忽略被并发删除的文件，否则报错 */
			if (errno == ENOENT)
				continue;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", path)));
		}

		/* 忽略普通文件以外的一切 */
		if (!S_ISREG(attrib.st_mode))
			continue;

		values[0] = CStringGetTextDatum(de->d_name);
		values[1] = Int64GetDatum((int64) attrib.st_size);
		values[2] = TimestampTzGetDatum(time_t_to_timestamptz(attrib.st_mtime));
		memset(nulls, 0, sizeof(nulls));

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	FreeDir(dirdesc);
	return (Datum) 0;
}

/* 返回日志目录中文件列表的函数 */
Datum
pg_ls_logdir(PG_FUNCTION_ARGS)
{
	return pg_ls_dir_files(fcinfo, Log_directory, false);
}

/* 返回 WAL 目录中文件列表的函数 */
Datum
pg_ls_waldir(PG_FUNCTION_ARGS)
{
	return pg_ls_dir_files(fcinfo, XLOGDIR, false);
}

/*
 * 返回 pgsql_tmp 中文件列表的通用函数
 */
static Datum
pg_ls_tmpdir(FunctionCallInfo fcinfo, Oid tblspc)
{
	char		path[MAXPGPATH];

	if (!SearchSysCacheExists1(TABLESPACEOID, ObjectIdGetDatum(tblspc)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace with OID %u does not exist",
						tblspc)));

	TempTablespacePath(path, tblspc);
	return pg_ls_dir_files(fcinfo, path, true);
}

/*
 * 返回 pg_default 表空间中临时文件列表的函数
 * pgsql_tmp 目录
 */
Datum
pg_ls_tmpdir_noargs(PG_FUNCTION_ARGS)
{
	return pg_ls_tmpdir(fcinfo, DEFAULTTABLESPACE_OID);
}

/*
 * 返回指定表空间中临时文件列表的函数
 * pgsql_tmp 目录
 */
Datum
pg_ls_tmpdir_1arg(PG_FUNCTION_ARGS)
{
	return pg_ls_tmpdir(fcinfo, PG_GETARG_OID(0));
}

/*
 * 返回 WAL 归档状态目录中文件列表的函数。
 */
Datum
pg_ls_archive_statusdir(PG_FUNCTION_ARGS)
{
	return pg_ls_dir_files(fcinfo, XLOGDIR "/archive_status", true);
}

/*
 * 返回 pg_logical/snapshots 目录中文件列表的函数。
 */
Datum
pg_ls_logicalsnapdir(PG_FUNCTION_ARGS)
{
	return pg_ls_dir_files(fcinfo, "pg_logical/snapshots", false);
}

/*
 * 返回 pg_logical/mappings 目录中文件列表的函数。
 */
Datum
pg_ls_logicalmapdir(PG_FUNCTION_ARGS)
{
	return pg_ls_dir_files(fcinfo, "pg_logical/mappings", false);
}

/*
 * 返回 pg_replslot/<replication_slot> 目录文件
 * 清单
 */
Datum
pg_ls_replslotdir(PG_FUNCTION_ARGS)
{
	text	   *slotname_t;
	char		path[MAXPGPATH];
	char	   *slotname;

	slotname_t = PG_GETARG_TEXT_PP(0);

	slotname = text_to_cstring(slotname_t);

	if (!SearchNamedReplicationSlot(slotname, true))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist",
						slotname)));

	snprintf(path, sizeof(path), "pg_replslot/%s", slotname);
	return pg_ls_dir_files(fcinfo, path, false);
}
