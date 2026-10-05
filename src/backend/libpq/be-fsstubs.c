/*-------------------------------------------------------------------------
 *
 * be-fsstubs.c
 *	  对大对象做 open/close/read/write 的内建函数
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/libpq/be-fsstubs.c
 *
 * 说明
 *	  本文件应当挪到更合适的地方。它放在这里
 *	  只是因为没有更好的去处。
 *
 *	  这些函数把 LargeObjectDesc 结构存放在私有 MemoryContext 中，
 *	  这意味着大对象描述符会一直留到我们在
 *	  事务结束时销毁该上下文。可以延长该上下文的
 *	  寿命，使大对象 FD 在跨事务之后仍然有效（例如，
 *	  仅当看到已没有 FD 保持打开时才释放上下文）。
 *	  但要做到中止事务结束时的正确处理，还需要
 *	  额外的状态。在已中止事务期间打开的 FD
 *	  仍必须关闭，因为它们也许根本没有指向有效的
 *	  关系。若大对象跨事务保持打开，加锁语义
 *	  也是一个有意思的问题。目前我们仍遵守
 *	  已经文档化的大对象 FD 语义：它们只在一个
 *	  事务之内有效。
 *
 *	  到 PostgreSQL 8.0，上面所说的大部分顾虑已经不再
 *	  重要，事实上让大对象 FD 跨事务保持
 *	  打开会相当容易。（快照是否仍然相关仍是问题。）
 *	  不过，向后兼容表明我们应当维持
 *	  现状。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/pg_largeobject.h"
#include "libpq/be-fsstubs.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/large_object.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "varatt.h"

/* 定义此项以启用调试日志 */
/* #define FSDB 1 */
/* lo_import/lo_export 传输用的块大小 */
#define BUFSIZE			8192

/*
 * LO "FD" 是 cookies 数组的下标。
 *
 * 非 NULL 条目是指向 LargeObjectDesc 的指针，分配在
 * 大对象私有内存上下文 "fscxt" 中。cookies 数组本身
 * 也在该上下文中动态分配。当前分配大小为
 * cookies_size 个条目，其中未使用的条目为 NULL。
 */
static LargeObjectDesc **cookies = NULL;
static int	cookies_size = 0;

static bool lo_cleanup_needed = false;
static MemoryContext fscxt = NULL;

static int	newLOfd(void);
static void closeLOfd(int fd);
static Oid	lo_import_internal(text *filename, Oid lobjOid);


/*****************************************************************************
 *	大对象的文件接口
 *****************************************************************************/

Datum
be_lo_open(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	int32		mode = PG_GETARG_INT32(1);
	LargeObjectDesc *lobjDesc;
	int			fd;

#ifdef FSDB
	elog(DEBUG4, "lo_open(%u,%d)", lobjId, mode);
#endif

	if (mode & INV_WRITE)
		PreventCommandIfReadOnly("lo_open(INV_WRITE)");

	/*
 * 先分配一个大对象描述符。若这是本事务中打开的
 * 第一个大对象，这也会创建 'fscxt'。
 */
	fd = newLOfd();

	lobjDesc = inv_open(lobjId, mode, fscxt);
	lobjDesc->subid = GetCurrentSubTransactionId();

	/*
 * 必须把快照登记到 TopTransaction 的 resowner 中，使它
 * 一直存活到大对象关闭，而不是在当前门户
 * 关闭时就结束。
 */
	if (lobjDesc->snapshot)
		lobjDesc->snapshot = RegisterSnapshotOnOwner(lobjDesc->snapshot,
													 TopTransactionResourceOwner);

	Assert(cookies[fd] == NULL);
	cookies[fd] = lobjDesc;

	PG_RETURN_INT32(fd);
}

Datum
be_lo_close(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

#ifdef FSDB
	elog(DEBUG4, "lo_close(%d)", fd);
#endif

	closeLOfd(fd);

	PG_RETURN_INT32(0);
}


/*****************************************************************************
 *	裸读/写操作 --- 这些不是可供 fmgr 调用的！
 *
 *	我们假定大对象支持面向字节的读取和定位，以便
 *	我们的工作更简单。
 *
 *****************************************************************************/

int
lo_read(int fd, char *buf, int len)
{
	int			status;
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/*
 * 检查状态。inv_read() 反正会抛错，但我们希望
 * 错误针对 FD 的状态，而不是底层权限；也许
 * 权限是有的，只是用户忘了请求读模式。
 */
	if ((lobj->flags & IFS_RDLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for reading",
						fd)));

	status = inv_read(lobj, buf, len);

	return status;
}

int
lo_write(int fd, const char *buf, int len)
{
	int			status;
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/* 参见 lo_read() 中的注释 */
	if ((lobj->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for writing",
						fd)));

	status = inv_write(lobj, buf, len);

	return status;
}

Datum
be_lo_lseek(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		offset = PG_GETARG_INT32(1);
	int32		whence = PG_GETARG_INT32(2);
	int64		status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	status = inv_seek(cookies[fd], offset, whence);

	/* 防止结果溢出 */
	if (status != (int32) status)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("lo_lseek result out of range for large-object descriptor %d",
						fd)));

	PG_RETURN_INT32((int32) status);
}

Datum
be_lo_lseek64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset = PG_GETARG_INT64(1);
	int32		whence = PG_GETARG_INT32(2);
	int64		status;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	status = inv_seek(cookies[fd], offset, whence);

	PG_RETURN_INT64(status);
}

Datum
be_lo_creat(PG_FUNCTION_ARGS)
{
	Oid			lobjId;

	PreventCommandIfReadOnly("lo_creat()");

	lo_cleanup_needed = true;
	lobjId = inv_create(InvalidOid);

	PG_RETURN_OID(lobjId);
}

Datum
be_lo_create(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);

	PreventCommandIfReadOnly("lo_create()");

	lo_cleanup_needed = true;
	lobjId = inv_create(lobjId);

	PG_RETURN_OID(lobjId);
}

Datum
be_lo_tell(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	offset = inv_tell(cookies[fd]);

	/* 防止结果溢出 */
	if (offset != (int32) offset)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("lo_tell result out of range for large-object descriptor %d",
						fd)));

	PG_RETURN_INT32((int32) offset);
}

Datum
be_lo_tell64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		offset;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));

	offset = inv_tell(cookies[fd]);

	PG_RETURN_INT64(offset);
}

Datum
be_lo_unlink(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);

	PreventCommandIfReadOnly("lo_unlink()");

	/*
 * 必须是大对象的所有者。在 inv_drop() 里检查会更干净，
 * 但我们希望在关闭相关 FD 之前抛错，
 * 而不是之后。
 */
	if (!lo_compat_privileges &&
		!object_ownercheck(LargeObjectRelationId, lobjId, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of large object %u", lobjId)));

	/*
 * 若有打开的大对象 FD 引用该 ID，就把它们关掉。
 */
	if (fscxt != NULL)
	{
		int			i;

		for (i = 0; i < cookies_size; i++)
		{
			if (cookies[i] != NULL && cookies[i]->id == lobjId)
				closeLOfd(i);
		}
	}

	/*
 * inv_drop 并不产生事务结束时清理的需要，
 * 因此不必设置 lo_cleanup_needed。
 */
	PG_RETURN_INT32(inv_drop(lobjId));
}

/*****************************************************************************
 *	使用 bytea 的读/写
 *****************************************************************************/

Datum
be_loread(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		len = PG_GETARG_INT32(1);
	bytea	   *retval;
	int			totalread;

	if (len < 0)
		len = 0;

	retval = (bytea *) palloc(VARHDRSZ + len);
	totalread = lo_read(fd, VARDATA(retval), len);
	SET_VARSIZE(retval, totalread + VARHDRSZ);

	PG_RETURN_BYTEA_P(retval);
}

Datum
be_lowrite(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	bytea	   *wbuf = PG_GETARG_BYTEA_PP(1);
	int			bytestowrite;
	int			totalwritten;

	PreventCommandIfReadOnly("lowrite()");

	bytestowrite = VARSIZE_ANY_EXHDR(wbuf);
	totalwritten = lo_write(fd, VARDATA_ANY(wbuf), bytestowrite);
	PG_RETURN_INT32(totalwritten);
}

/*****************************************************************************
 *	大对象的导入/导出
 *****************************************************************************/

/*
 * lo_import -
 *	  把文件导入为（inversion）大对象。
 */
Datum
be_lo_import(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_PP(0);

	PG_RETURN_OID(lo_import_internal(filename, InvalidOid));
}

/*
 * lo_import_with_oid -
 *	  按指定 oid 把文件导入为（inversion）大对象。
 */
Datum
be_lo_import_with_oid(PG_FUNCTION_ARGS)
{
	text	   *filename = PG_GETARG_TEXT_PP(0);
	Oid			oid = PG_GETARG_OID(1);

	PG_RETURN_OID(lo_import_internal(filename, oid));
}

static Oid
lo_import_internal(text *filename, Oid lobjOid)
{
	int			fd;
	int			nbytes,
				tmp PG_USED_FOR_ASSERTS_ONLY;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	Oid			oid;

	PreventCommandIfReadOnly("lo_import()");

	/*
 *	打开要读入的文件
 */
	text_to_cstring_buffer(filename, fnamebuf, sizeof(fnamebuf));
	fd = OpenTransientFile(fnamebuf, O_RDONLY | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open server file \"%s\": %m",
						fnamebuf)));

	/*
 *	创建一个 inversion 对象
 */
	lo_cleanup_needed = true;
	oid = inv_create(lobjOid);

	/*
 *	从文件系统读入并写入 inversion 对象
 */
	lobj = inv_open(oid, INV_WRITE, CurrentMemoryContext);

	while ((nbytes = read(fd, buf, BUFSIZE)) > 0)
	{
		tmp = inv_write(lobj, buf, nbytes);
		Assert(tmp == nbytes);
	}

	if (nbytes < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read server file \"%s\": %m",
						fnamebuf)));

	inv_close(lobj);

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						fnamebuf)));

	return oid;
}

/*
 * lo_export -
 *	  导出一个（inversion）大对象。
 */
Datum
be_lo_export(PG_FUNCTION_ARGS)
{
	Oid			lobjId = PG_GETARG_OID(0);
	text	   *filename = PG_GETARG_TEXT_PP(1);
	int			fd;
	int			nbytes,
				tmp;
	char		buf[BUFSIZE];
	char		fnamebuf[MAXPGPATH];
	LargeObjectDesc *lobj;
	mode_t		oumask;

	/*
 *	打开 inversion 对象（不必测试是否失败）
 */
	lo_cleanup_needed = true;
	lobj = inv_open(lobjId, INV_READ, CurrentMemoryContext);

	/*
 *	打开要写入的文件
 *
 *	注意：我们把后端通常的 077 umask 降到稍友好一些的
 *	022。这段代码曾经把它一路降到 0，但创建
 *	任何人都可写的导出文件似乎并不明智。
 */
	text_to_cstring_buffer(filename, fnamebuf, sizeof(fnamebuf));
	oumask = umask(S_IWGRP | S_IWOTH);
	PG_TRY();
	{
		fd = OpenTransientFilePerm(fnamebuf, O_CREAT | O_WRONLY | O_TRUNC | PG_BINARY,
								   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}
	PG_FINALLY();
	{
		umask(oumask);
	}
	PG_END_TRY();
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create server file \"%s\": %m",
						fnamebuf)));

	/*
 *	从 inversion 文件读入并写入文件系统
 */
	while ((nbytes = inv_read(lobj, buf, BUFSIZE)) > 0)
	{
		tmp = write(fd, buf, nbytes);
		if (tmp != nbytes)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write server file \"%s\": %m",
							fnamebuf)));
	}

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m",
						fnamebuf)));

	inv_close(lobj);

	PG_RETURN_INT32(1);
}

/*
 * lo_truncate -
 *	  把大对象截断到指定长度
 */
static void
lo_truncate_internal(int32 fd, int64 len)
{
	LargeObjectDesc *lobj;

	if (fd < 0 || fd >= cookies_size || cookies[fd] == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("invalid large-object descriptor: %d", fd)));
	lobj = cookies[fd];

	/* 参见 lo_read() 中的注释 */
	if ((lobj->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("large object descriptor %d was not opened for writing",
						fd)));

	inv_truncate(lobj, len);
}

Datum
be_lo_truncate(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int32		len = PG_GETARG_INT32(1);

	PreventCommandIfReadOnly("lo_truncate()");

	lo_truncate_internal(fd, len);
	PG_RETURN_INT32(0);
}

Datum
be_lo_truncate64(PG_FUNCTION_ARGS)
{
	int32		fd = PG_GETARG_INT32(0);
	int64		len = PG_GETARG_INT64(1);

	PreventCommandIfReadOnly("lo_truncate64()");

	lo_truncate_internal(fd, len);
	PG_RETURN_INT32(0);
}

/*
 * AtEOXact_LargeObject -
 *	  为事务提交准备大对象
 */
void
AtEOXact_LargeObject(bool isCommit)
{
	int			i;

	if (!lo_cleanup_needed)
		return;					/* 本事务中没有大对象操作 */

	/*
 * 关闭大对象 FD 并清空 cookies 数组，使大对象 FD 不再有效。
 * 持有它们的内存上下文和资源所有者反正会在
 * 事务结束时消失，但在提交时需要关闭它们，以免
 * 提交时出现资源泄漏警告。中止时可以跳过
 * 这一步。
 */
	if (isCommit)
	{
		for (i = 0; i < cookies_size; i++)
		{
			if (cookies[i] != NULL)
				closeLOfd(i);
		}
	}

	/* 不必真的 pfree，因为马上就要清掉上下文 */
	cookies = NULL;
	cookies_size = 0;

	/* 释放大对象内存上下文，防止永久性内存泄漏。 */
	if (fscxt)
		MemoryContextDelete(fscxt);
	fscxt = NULL;

	/* 也给 inv_api.c 一个清理的机会 */
	close_lo_relation(isCommit);

	lo_cleanup_needed = false;
}

/*
 * AtEOSubXact_LargeObject
 *	  在子事务提交/中止时处理大对象
 *
 * 把在正在提交的子事务期间创建或打开的大对象
 * 重新归到父级子事务。中止时只关闭它们。
 */
void
AtEOSubXact_LargeObject(bool isCommit, SubTransactionId mySubid,
						SubTransactionId parentSubid)
{
	int			i;

	if (fscxt == NULL)			/* 本事务中没有大对象操作 */
		return;

	for (i = 0; i < cookies_size; i++)
	{
		LargeObjectDesc *lo = cookies[i];

		if (lo != NULL && lo->subid == mySubid)
		{
			if (isCommit)
				lo->subid = parentSubid;
			else
				closeLOfd(i);
		}
	}
}

/*****************************************************************************
 *	本文件的支持例程
 *****************************************************************************/

static int
newLOfd(void)
{
	int			i,
				newsize;

	lo_cleanup_needed = true;
	if (fscxt == NULL)
		fscxt = AllocSetContextCreate(TopMemoryContext,
									  "Filesystem",
									  ALLOCSET_DEFAULT_SIZES);

	/* 试着找一个空闲槽位 */
	for (i = 0; i < cookies_size; i++)
	{
		if (cookies[i] == NULL)
			return i;
	}

	/* 没有空闲槽位，于是把数组加大 */
	if (cookies_size <= 0)
	{
		/* 第一次进入时，随意做成 64 个元素的数组 */
		i = 0;
		newsize = 64;
		cookies = (LargeObjectDesc **)
			MemoryContextAllocZero(fscxt, newsize * sizeof(LargeObjectDesc *));
	}
	else
	{
		/* 把数组大小加倍 */
		i = cookies_size;
		newsize = cookies_size * 2;
		cookies =
			repalloc0_array(cookies, LargeObjectDesc *, cookies_size, newsize);
	}
	cookies_size = newsize;

	return i;
}

static void
closeLOfd(int fd)
{
	LargeObjectDesc *lobj;

	/*
 * 若这里因某种原因出错，确保不会尝试释放两次。
 * 泄漏总比崩溃好。
 */
	lobj = cookies[fd];
	cookies[fd] = NULL;

	if (lobj->snapshot)
		UnregisterSnapshotFromOwner(lobj->snapshot,
									TopTransactionResourceOwner);
	inv_close(lobj);
}

/*****************************************************************************
 *	面向 SQL 调用方的包装
 *****************************************************************************/

/*
 * 读取大对象中 [offset, offset+nbytes) 这一段；nbytes 为 -1 时读到末尾。
 */
static bytea *
lo_get_fragment_internal(Oid loOid, int64 offset, int32 nbytes)
{
	LargeObjectDesc *loDesc;
	int64		loSize;
	int64		result_length;
	int			total_read PG_USED_FOR_ASSERTS_ONLY;
	bytea	   *result = NULL;

	lo_cleanup_needed = true;
	loDesc = inv_open(loOid, INV_READ, CurrentMemoryContext);

	/*
 * 计算实际要读取的字节数，兼顾 nbytes == -1
 * 以及越过大对象末尾的读取。
 */
	loSize = inv_seek(loDesc, 0, SEEK_END);
	if (loSize > offset)
	{
		if (nbytes >= 0 && nbytes <= loSize - offset)
			result_length = nbytes; /* 请求完全落在大对象内部 */
		else
			result_length = loSize - offset;	/* 调整到大对象末尾 */
	}
	else
		result_length = 0;		/* 请求完全落在大对象外部 */

	/*
 * 由 loSize 算出的 result_length 也许放不进 size_t。检查
 * 该大小能否满足这一点以及随后强制的大小限制。
 */
	if (result_length > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("large object read request is too large")));

	result = (bytea *) palloc(VARHDRSZ + result_length);

	inv_seek(loDesc, offset, SEEK_SET);
	total_read = inv_read(loDesc, VARDATA(result), result_length);
	Assert(total_read == result_length);
	SET_VARSIZE(result, result_length + VARHDRSZ);

	inv_close(loDesc);

	return result;
}

/*
 * 读取整个大对象
 */
Datum
be_lo_get(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	bytea	   *result;

	result = lo_get_fragment_internal(loOid, 0, -1);

	PG_RETURN_BYTEA_P(result);
}

/*
 * 读取大对象中的一段范围
 */
Datum
be_lo_get_fragment(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	int64		offset = PG_GETARG_INT64(1);
	int32		nbytes = PG_GETARG_INT32(2);
	bytea	   *result;

	if (nbytes < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("requested length cannot be negative")));

	result = lo_get_fragment_internal(loOid, offset, nbytes);

	PG_RETURN_BYTEA_P(result);
}

/*
 * 用 bytea 参数给出的初始内容创建大对象
 */
Datum
be_lo_from_bytea(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	bytea	   *str = PG_GETARG_BYTEA_PP(1);
	LargeObjectDesc *loDesc;
	int			written PG_USED_FOR_ASSERTS_ONLY;

	PreventCommandIfReadOnly("lo_from_bytea()");

	lo_cleanup_needed = true;
	loOid = inv_create(loOid);
	loDesc = inv_open(loOid, INV_WRITE, CurrentMemoryContext);
	written = inv_write(loDesc, VARDATA_ANY(str), VARSIZE_ANY_EXHDR(str));
	Assert(written == VARSIZE_ANY_EXHDR(str));
	inv_close(loDesc);

	PG_RETURN_OID(loOid);
}

/*
 * 更新大对象中的一段范围
 */
Datum
be_lo_put(PG_FUNCTION_ARGS)
{
	Oid			loOid = PG_GETARG_OID(0);
	int64		offset = PG_GETARG_INT64(1);
	bytea	   *str = PG_GETARG_BYTEA_PP(2);
	LargeObjectDesc *loDesc;
	int			written PG_USED_FOR_ASSERTS_ONLY;

	PreventCommandIfReadOnly("lo_put()");

	lo_cleanup_needed = true;
	loDesc = inv_open(loOid, INV_WRITE, CurrentMemoryContext);

	/* 权限检查 */
	if (!lo_compat_privileges &&
		pg_largeobject_aclcheck_snapshot(loDesc->id,
										 GetUserId(),
										 ACL_UPDATE,
										 loDesc->snapshot) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						loDesc->id)));

	inv_seek(loDesc, offset, SEEK_SET);
	written = inv_write(loDesc, VARDATA_ANY(str), VARSIZE_ANY_EXHDR(str));
	Assert(written == VARSIZE_ANY_EXHDR(str));
	inv_close(loDesc);

	PG_RETURN_VOID();
}
