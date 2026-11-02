/*-------------------------------------------------------------------------
 *
 * fe-lobj.c
 *	  Front-end large object interface
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-lobj.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef WIN32
/*
 *	As unlink/rename are #define'd in port.h (via postgres_fe.h), io.h
 *	must be included first on MS C.  Might as well do it for all WIN32's
 *	here.
 */
#include <io.h>
#endif

#include "postgres_fe.h"

#ifdef WIN32
#include "win32.h"
#else
#include <unistd.h>
#endif

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "libpq/libpq-fs.h"		/* must come after sys/stat.h */
#include "port/pg_bswap.h"

#define LO_BUFSIZE		  8192

static int	lo_initialize(PGconn *conn);
static Oid	lo_import_internal(PGconn *conn, const char *filename, Oid oid);
static pg_int64 lo_hton64(pg_int64 host64);
static pg_int64 lo_ntoh64(pg_int64 net64);

/*lo_open
 * 	  打开一个现有的大对象
 *	
 *返回文件描述符供后续 lo_* 调用使用
 * 失败时返回 -1。
 * 
         */
int
lo_open(PGconn *conn, Oid lobjId, int mode)
{
	int			fd;
	int			result_len;
	PQArgBlock	argv[2];
	PGresult   *res;

	if (lo_initialize(conn) < 0)
		return -1;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = mode;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_open, &fd, &result_len, 1, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return fd;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_close
 * 	  关闭一个现有的大对象
 *	
 *成功时返回 0
 * 失败时返回 -1。
 * 
         */
int
lo_close(PGconn *conn, int fd)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_close,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_truncate
 * 	  将现有大对象截断到给定大小
 *	
 *成功时返回 0
 * 失败时返回 -1
 * 
                       */
int
lo_truncate(PGconn *conn, int fd, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	/* 必须动态检查，因为 8.3 之前没有此函数     */
	if (conn->lobjfuncs->fn_lo_truncate == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_truncate");
		return -1;
	}

	/*很久以前，有人觉得将此函数声明为接收 size_t 是个好主意……
	 * 但底层后端函数只接受有符号 int32 长度。因此如果给定值溢出
	 * int32 则抛出错误。（一个可能的替代方案是自动将调用重定向到
	 * lo_truncate64；但如果调用者想要依赖该后端函数可用，他本可以
	 * 自己调用 lo_truncate64。）
	 * 
	 * 
	 * 
	                                                     */
	if (len > (size_t) INT_MAX)
	{
		libpq_append_conn_error(conn, "argument of lo_truncate exceeds integer range");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = (int) len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_truncate,
			   &retval, &result_len, 1, argv, 2);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_truncate64
 * 	  将现有大对象截断到给定大小
 *	
 *成功时返回 0
 * 失败时返回 -1
 * 
                       */
int
lo_truncate64(PGconn *conn, int fd, pg_int64 len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	if (conn->lobjfuncs->fn_lo_truncate64 == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_truncate64");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	len = lo_hton64(len);
	argv[1].isint = 0;
	argv[1].len = 8;
	argv[1].u.ptr = (int *) &len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_truncate64,
			   &retval, &result_len, 1, argv, 2);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_read
 * 	  从大对象中读取 len 字节到 buf
 *	
 *返回读取的字节数，失败时返回 -1。
 * 调用者必须已分配足够空间来保存返回的结果
 * 
                  */

int
lo_read(PGconn *conn, int fd, char *buf, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	/*很久以前，有人觉得将此函数声明为接收 size_t 是个好主意……
	 * 但底层后端函数只接受有符号 int32 长度。因此如果给定值溢出
	 * int32 则抛出错误。
	 * 
	 * 
	                         */
	if (len > (size_t) INT_MAX)
	{
		libpq_append_conn_error(conn, "argument of lo_read exceeds integer range");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = (int) len;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_read,
			   (void *) buf, &result_len, 0, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return result_len;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_write
 * 	  将 buf 的 len 字节写入大对象 fd
 *	
 *返回写入的字节数，失败时返回 -1。
 * 
             */
int
lo_write(PGconn *conn, int fd, const char *buf, size_t len)
{
	PQArgBlock	argv[2];
	PGresult   *res;
	int			result_len;
	int			retval;

	if (lo_initialize(conn) < 0)
		return -1;

	/*很久以前，有人觉得将此函数声明为接收 size_t 是个好主意……
	 * 但底层后端函数只接受有符号 int32 长度。因此如果给定值溢出
	 * int32 则抛出错误。
	 * 
	 * 
	                         */
	if (len > (size_t) INT_MAX)
	{
		libpq_append_conn_error(conn, "argument of lo_write exceeds integer range");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 0;
	argv[1].len = (int) len;
	argv[1].u.ptr = (int *) unconstify(char *, buf);

	res = PQfn(conn, conn->lobjfuncs->fn_lo_write,
			   &retval, &result_len, 1, argv, 2);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_lseek
 * 	  更改大对象上的当前读写位置
 *	
                    */
int
lo_lseek(PGconn *conn, int fd, int offset, int whence)
{
	PQArgBlock	argv[3];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	argv[1].isint = 1;
	argv[1].len = 4;
	argv[1].u.integer = offset;

	argv[2].isint = 1;
	argv[2].len = 4;
	argv[2].u.integer = whence;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_lseek,
			   &retval, &result_len, 1, argv, 3);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_lseek64
 * 	  更改大对象上的当前读写位置
 *	
                    */
pg_int64
lo_lseek64(PGconn *conn, int fd, pg_int64 offset, int whence)
{
	PQArgBlock	argv[3];
	PGresult   *res;
	pg_int64	retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	if (conn->lobjfuncs->fn_lo_lseek64 == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_lseek64");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	offset = lo_hton64(offset);
	argv[1].isint = 0;
	argv[1].len = 8;
	argv[1].u.ptr = (int *) &offset;

	argv[2].isint = 1;
	argv[2].len = 4;
	argv[2].u.integer = whence;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_lseek64,
			   (void *) &retval, &result_len, 0, argv, 3);
	if (PQresultStatus(res) == PGRES_COMMAND_OK && result_len == 8)
	{
		PQclear(res);
		return lo_ntoh64(retval);
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_creat
 * 	  创建一个新的大对象
 *	mode 被忽略（曾经有过用途）
 * 
 *返回创建的大对象的 OID，或失败时返回
 * InvalidOid
 * 
                  */
Oid
lo_creat(PGconn *conn, int mode)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return InvalidOid;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = mode;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_creat,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return (Oid) retval;
	}
	else
	{
		PQclear(res);
		return InvalidOid;
	}
}

/*lo_create
 * 	  创建一个新的大对象
 *	如果 lobjId 不是 InvalidOid，则指定要（尝试）创建的 OID
 * 
 *返回创建的大对象的 OID，或失败时返回
 * InvalidOid
 * 
     */
Oid
lo_create(PGconn *conn, Oid lobjId)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			retval;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return InvalidOid;

	/* 必须动态检查，因为 8.1 之前没有此函数     */
	if (conn->lobjfuncs->fn_lo_create == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_create");
		return InvalidOid;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;
	res = PQfn(conn, conn->lobjfuncs->fn_lo_create,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return (Oid) retval;
	}
	else
	{
		PQclear(res);
		return InvalidOid;
	}
}


/*lo_tell
 * 	  返回大对象的当前查找位置
 *	
                 */
int
lo_tell(PGconn *conn, int fd)
{
	int			retval;
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_tell,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_tell64
 * 	  返回大对象的当前查找位置
 *	
                 */
pg_int64
lo_tell64(PGconn *conn, int fd)
{
	pg_int64	retval;
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;

	if (lo_initialize(conn) < 0)
		return -1;

	if (conn->lobjfuncs->fn_lo_tell64 == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_tell64");
		return -1;
	}

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = fd;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_tell64,
			   (void *) &retval, &result_len, 0, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK && result_len == 8)
	{
		PQclear(res);
		return lo_ntoh64(retval);
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*
 * lo_unlink
 *	  delete a file
 */

int
lo_unlink(PGconn *conn, Oid lobjId)
{
	PQArgBlock	argv[1];
	PGresult   *res;
	int			result_len;
	int			retval;

	if (lo_initialize(conn) < 0)
		return -1;

	argv[0].isint = 1;
	argv[0].len = 4;
	argv[0].u.integer = lobjId;

	res = PQfn(conn, conn->lobjfuncs->fn_lo_unlink,
			   &retval, &result_len, 1, argv, 1);
	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return retval;
	}
	else
	{
		PQclear(res);
		return -1;
	}
}

/*lo_import -
 * 	  将文件导入为（反转）大对象。
 *	
 *成功时返回该对象的 OID，
 * 失败时返回 InvalidOid
 * 
                   */

Oid
lo_import(PGconn *conn, const char *filename)
{
	return lo_import_internal(conn, filename, InvalidOid);
}

/*lo_import_with_oid -
 * 	  将文件导入为（反转）大对象。
 *	  可以指定大对象 ID。
 *	
 *成功时返回该对象的 OID，
 * 失败时返回 InvalidOid
 * 
                         */

Oid
lo_import_with_oid(PGconn *conn, const char *filename, Oid lobjId)
{
	return lo_import_internal(conn, filename, lobjId);
}

static Oid
lo_import_internal(PGconn *conn, const char *filename, Oid oid)
{
	int			fd;
	int			nbytes,
				tmp;
	char		buf[LO_BUFSIZE];
	Oid			lobjOid;
	int			lobj;
	char		sebuf[PG_STRERROR_R_BUFLEN];

	if (conn == NULL)
		return InvalidOid;

	/* 由于这是查询周期的开始，重置错误状态              */
	pqClearConnErrorState(conn);

	/*打开要读取的文件
	 * 
	    */
	fd = open(filename, O_RDONLY | PG_BINARY, 0666);
	if (fd < 0)
	{							/* error */
		libpq_append_conn_error(conn, "could not open file \"%s\": %s",
								filename, strerror_r(errno, sebuf, sizeof(sebuf)));
		return InvalidOid;
	}

	/*创建一个反转对象
	 * 
	   */
	if (oid == InvalidOid)
		lobjOid = lo_creat(conn, INV_READ | INV_WRITE);
	else
		lobjOid = lo_create(conn, oid);

	if (lobjOid == InvalidOid)
	{
		/* 我们假定 lo_create() 已设置了适当的错误消息 */
		(void) close(fd);
		return InvalidOid;
	}

	lobj = lo_open(conn, lobjOid, INV_WRITE);
	if (lobj == -1)
	{
		/* 我们假定 lo_open() 已设置了适当的错误消息 */
		(void) close(fd);
		return InvalidOid;
	}

	/*从文件读取并写入大对象
	 * 
	                   */
	while ((nbytes = read(fd, buf, LO_BUFSIZE)) > 0)
	{
		tmp = lo_write(conn, lobj, buf, nbytes);
		if (tmp != nbytes)
		{
			/*如果 lo_write() 失败，我们现在处于已中止事务中，因此
	不需要 lo_close()；而且，如果尝试调用它，我们会用无用的
	错误结果覆盖有用的错误结果。所以直接关门走人。
	
	
	                               */
			(void) close(fd);
			return InvalidOid;
		}
	}

	if (nbytes < 0)
	{
		/* We must do lo_close before setting the errorMessage */
		int			save_errno = errno;

		(void) lo_close(conn, lobj);
		(void) close(fd);
		/* 故意覆盖来自 lo_close 的任何错误    */
		pqClearConnErrorState(conn);
		libpq_append_conn_error(conn, "could not read from file \"%s\": %s",
								filename,
								strerror_r(save_errno, sebuf, sizeof(sebuf)));
		return InvalidOid;
	}

	(void) close(fd);

	if (lo_close(conn, lobj) != 0)
	{
		/* 我们假定 lo_close() 已设置了适当的错误消息 */
		return InvalidOid;
	}

	return lobjOid;
}

/*
 * lo_export -
 *	  exports an (inversion) large object.
 * returns -1 upon failure, 1 if OK
 */
int
lo_export(PGconn *conn, Oid lobjId, const char *filename)
{
	int			result = 1;
	int			fd;
	int			nbytes,
				tmp;
	char		buf[LO_BUFSIZE];
	int			lobj;
	char		sebuf[PG_STRERROR_R_BUFLEN];

	/*打开大对象。
	 * 
	     */
	lobj = lo_open(conn, lobjId, INV_READ);
	if (lobj == -1)
	{
		/* 我们假定 lo_open() 已设置了适当的错误消息 */
		return -1;
	}

	/*创建要写入的文件
	 * 
	         */
	fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY, 0666);
	if (fd < 0)
	{
		/* We must do lo_close before setting the errorMessage */
		int			save_errno = errno;

		(void) lo_close(conn, lobj);
		/* 故意覆盖来自 lo_close 的任何错误    */
		pqClearConnErrorState(conn);
		libpq_append_conn_error(conn, "could not open file \"%s\": %s",
								filename,
								strerror_r(save_errno, sebuf, sizeof(sebuf)));
		return -1;
	}

	/*
	 * read in from the large object and write to the file
	 */
	while ((nbytes = lo_read(conn, lobj, buf, LO_BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp != nbytes)
		{
			/* We must do lo_close before setting the errorMessage */
			int			save_errno = errno;

			(void) lo_close(conn, lobj);
			(void) close(fd);
			/* 故意覆盖来自 lo_close 的任何错误    */
			pqClearConnErrorState(conn);
			libpq_append_conn_error(conn, "could not write to file \"%s\": %s",
									filename,
									strerror_r(save_errno, sebuf, sizeof(sebuf)));
			return -1;
		}
	}

	/*
	 * If lo_read() failed, we are now in an aborted transaction so there's no
	 * need for lo_close(); furthermore, if we tried it we'd overwrite the
	 * useful error result with a useless one. So skip lo_close() if we got a
	 * failure result.
	 */
	if (nbytes < 0 ||
		lo_close(conn, lobj) != 0)
	{
		/* assume lo_read() or lo_close() left a suitable error message */
		result = -1;
	}

	/* 如果已经失败，不要用 close 错误覆盖该消息        */
	if (close(fd) != 0 && result >= 0)
	{
		libpq_append_conn_error(conn, "could not write to file \"%s\": %s",
								filename, strerror_r(errno, sebuf, sizeof(sebuf)));
		result = -1;
	}

	return result;
}


/*lo_initialize
 * 
 *为现有连接上的新大对象操作进行初始化。
 * 成功时返回 0，失败时返回 -1。
 * 
 *如果我们之前没有这样做，则从 pg_proc 收集大对象操作所需的
 * 所有函数的 OID。
 * 
                                       */
static int
lo_initialize(PGconn *conn)
{
	PGresult   *res;
	PGlobjfuncs *lobjfuncs;
	int			n;
	const char *query;
	const char *fname;
	Oid			foid;

	/* Nothing we can do with no connection */
	if (conn == NULL)
		return -1;

	/* 由于这是查询周期的开始，重置错误状态              */
	pqClearConnErrorState(conn);

	/* Nothing else to do if we already collected info */
	if (conn->lobjfuncs != NULL)
		return 0;

	/*分配结构来保存函数 OID。在成功填充之前，我们不将其存储到
	 * PGconn 中。
	 * 
	                     */
	lobjfuncs = (PGlobjfuncs *) malloc(sizeof(PGlobjfuncs));
	if (lobjfuncs == NULL)
	{
		libpq_append_conn_error(conn, "out of memory");
		return -1;
	}
	MemSet((char *) lobjfuncs, 0, sizeof(PGlobjfuncs));

	/*
	 * Execute the query to get all the functions at once.  (Not all of them
	 * may exist in older server versions.)
	 */
	query = "select proname, oid from pg_catalog.pg_proc "
		"where proname in ("
		"'lo_open', "
		"'lo_close', "
		"'lo_creat', "
		"'lo_create', "
		"'lo_unlink', "
		"'lo_lseek', "
		"'lo_lseek64', "
		"'lo_tell', "
		"'lo_tell64', "
		"'lo_truncate', "
		"'lo_truncate64', "
		"'loread', "
		"'lowrite') "
		"and pronamespace = (select oid from pg_catalog.pg_namespace "
		"where nspname = 'pg_catalog')";

	res = PQexec(conn, query);
	if (res == NULL)
	{
		free(lobjfuncs);
		return -1;
	}

	if (res->resultStatus != PGRES_TUPLES_OK)
	{
		free(lobjfuncs);
		PQclear(res);
		libpq_append_conn_error(conn, "query to initialize large object functions did not return data");
		return -1;
	}

	/*检查结果并将 OID 放入结构中
	 * 
	               */
	for (n = 0; n < PQntuples(res); n++)
	{
		fname = PQgetvalue(res, n, 0);
		foid = (Oid) atoi(PQgetvalue(res, n, 1));
		if (strcmp(fname, "lo_open") == 0)
			lobjfuncs->fn_lo_open = foid;
		else if (strcmp(fname, "lo_close") == 0)
			lobjfuncs->fn_lo_close = foid;
		else if (strcmp(fname, "lo_creat") == 0)
			lobjfuncs->fn_lo_creat = foid;
		else if (strcmp(fname, "lo_create") == 0)
			lobjfuncs->fn_lo_create = foid;
		else if (strcmp(fname, "lo_unlink") == 0)
			lobjfuncs->fn_lo_unlink = foid;
		else if (strcmp(fname, "lo_lseek") == 0)
			lobjfuncs->fn_lo_lseek = foid;
		else if (strcmp(fname, "lo_lseek64") == 0)
			lobjfuncs->fn_lo_lseek64 = foid;
		else if (strcmp(fname, "lo_tell") == 0)
			lobjfuncs->fn_lo_tell = foid;
		else if (strcmp(fname, "lo_tell64") == 0)
			lobjfuncs->fn_lo_tell64 = foid;
		else if (strcmp(fname, "lo_truncate") == 0)
			lobjfuncs->fn_lo_truncate = foid;
		else if (strcmp(fname, "lo_truncate64") == 0)
			lobjfuncs->fn_lo_truncate64 = foid;
		else if (strcmp(fname, "loread") == 0)
			lobjfuncs->fn_lo_read = foid;
		else if (strcmp(fname, "lowrite") == 0)
			lobjfuncs->fn_lo_write = foid;
	}

	PQclear(res);

	/*最后检查是否获取了所有必需的大对象接口函数
	 * （晚于石器时代添加的函数仅在使用时检查）
	 * 
	 * 
	                                 */
	if (lobjfuncs->fn_lo_open == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_open");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_close == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_close");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_creat == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_creat");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_unlink == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_unlink");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_lseek == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_lseek");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_tell == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lo_tell");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_read == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"loread");
		free(lobjfuncs);
		return -1;
	}
	if (lobjfuncs->fn_lo_write == 0)
	{
		libpq_append_conn_error(conn, "cannot determine OID of function %s",
								"lowrite");
		free(lobjfuncs);
		return -1;
	}

	/*将结构放入连接控制中
	 * 
	                */
	conn->lobjfuncs = lobjfuncs;
	return 0;
}

/*lo_hton64
 * 	  将 64 位整数从主机字节序转换为网络字节序
 *	
          */
static pg_int64
lo_hton64(pg_int64 host64)
{
	union
	{
		pg_int64	i64;
		uint32		i32[2];
	}			swap;
	uint32		t;

	/* High order half first, since we're doing MSB-first */
	t = (uint32) (host64 >> 32);
	swap.i32[0] = pg_hton32(t);

	/* Now the low order half */
	t = (uint32) host64;
	swap.i32[1] = pg_hton32(t);

	return swap.i64;
}

/*lo_ntoh64
 * 	  将 64 位整数从网络字节序转换为主机字节序
 *	
          */
static pg_int64
lo_ntoh64(pg_int64 net64)
{
	union
	{
		pg_int64	i64;
		uint32		i32[2];
	}			swap;
	pg_int64	result;

	swap.i64 = net64;

	result = (uint32) pg_ntoh32(swap.i32[0]);
	result <<= 32;
	result |= (uint32) pg_ntoh32(swap.i32[1]);

	return result;
}
