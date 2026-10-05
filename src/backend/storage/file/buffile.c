/*-------------------------------------------------------------------------
 *
 * buffile.c
 *	  Management of large buffered temporary files.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/buffile.c
 *
 * NOTES:
 *
 * BufFiles provide a very incomplete emulation of stdio atop virtual Files
 * (as managed by fd.c).  Currently, we only support the buffered-I/O
 * aspect of stdio: a read or write of the low-level File occurs only
 * when the buffer is filled or emptied.  This is an even bigger win
 * for virtual Files than for ordinary kernel files, since reducing the
 * frequency with which a virtual File is touched reduces "thrashing"
 * of opening/closing file descriptors.
 *
 * Note that BufFile structs are allocated with palloc(), and therefore
 * will go away automatically at query/transaction end.  Since the underlying
 * virtual Files are made with OpenTemporaryFile, all resources for
 * the file are certain to be cleaned up even if processing is aborted
 * by ereport(ERROR).  The data structures required are made in the
 * palloc context that was current when the BufFile was created, and
 * any external resources such as temp files are owned by the ResourceOwner
 * that was current at that time.
 *
 * BufFile also supports temporary files that exceed the OS file size limit
 * (by opening multiple fd.c temporary files).  This is an essential feature
 * for sorts and hashjoins on large amounts of data.
 *
 * BufFile supports temporary files that can be shared with other backends, as
 * infrastructure for parallel execution.  Such files need to be created as a
 * member of a SharedFileSet that all participants are attached to.
 *
 * BufFile also supports temporary files that can be used by the single backend
 * when the corresponding files need to be survived across the transaction and
 * need to be opened and closed multiple times.  Such files need to be created
 * as a member of a FileSet.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/buf_internals.h"
#include "storage/buffile.h"
#include "storage/fd.h"
#include "utils/resowner.h"

/*
 * 不论 RELSEG_SIZE 如何，我们都把 BufFiles 切成吉字节大小的段。
 * 原因是希望大型 BufFiles 在有多个表空间可用时
 * 分布到这些表空间上。
 */
#define MAX_PHYSICAL_FILESIZE	0x40000000
#define BUFFILE_SEG_SIZE		(MAX_PHYSICAL_FILESIZE / BLCKSZ)

/*
 * 这个数据结构表示一个带缓冲的文件，它由一个或
 * 多个物理文件组成（每个都通过 fd.c 管理的
 * 虚拟文件描述符来访问）。
 */
struct BufFile
{
	int			numFiles;		/* 集合中物理文件的个数 */
	/* 除最后一个文件外，长度都恰好是 MAX_PHYSICAL_FILESIZE */
	File	   *files;			/* 用 palloc 分配的数组，有 numFiles 个元素 */

	bool		isInterXact;	/* 是否跨事务保持打开？ */
	bool		dirty;			/* 缓冲区是否需要写回？ */
	bool		readOnly;		/* 该文件是否已被设为只读？ */

	FileSet    *fileset;		/* 基于文件集的段文件所用的空间 */
	const char *name;			/* 基于文件集的 BufFile 的名字 */

	/*
	 * resowner 是底层临时文件使用的 ResourceOwner。（我们
	 * 不需要显式记住正在使用的内存上下文，
	 * 因为创建之后只把数组 repalloc 得更大。）
	 */
	ResourceOwner resowner;

	/*
	 * current pos 是缓冲区起点在逻辑文件中的位置。
	 * BufFile 的使用者看到的位置是（curFile，curOffset + pos）。
	 */
	int			curFile;		/* 当前位置的文件下标（0..n）部分 */
	off_t		curOffset;		/* 当前位置的偏移部分 */
	int			pos;			/* 缓冲区中下一次读/写的位置 */
	int			nbytes;			/* 缓冲区中有效字节的总数 */

	/*
	 * XXX：理想情况下应当使用 PGIOAlignedBlock，但可能需要一种办法，
	 * 避免某些用户创建很多文件时浪费每个文件的对齐填充。
	 */
	PGAlignedBlock buffer;
};

static BufFile *makeBufFileCommon(int nfiles);
static BufFile *makeBufFile(File firstfile);
static void extendBufFile(BufFile *file);
static void BufFileLoadBuffer(BufFile *file);
static void BufFileDumpBuffer(BufFile *file);
static void BufFileFlush(BufFile *file);
static File MakeNewFileSetSegment(BufFile *buffile, int segment);

/*
 * 创建 BufFile 并做公共的初始化。
 */
static BufFile *
makeBufFileCommon(int nfiles)
{
	BufFile    *file = (BufFile *) palloc(sizeof(BufFile));

	file->numFiles = nfiles;
	file->isInterXact = false;
	file->dirty = false;
	file->resowner = CurrentResourceOwner;
	file->curFile = 0;
	file->curOffset = 0;
	file->pos = 0;
	file->nbytes = 0;

	return file;
}

/*
 * 根据第一个底层物理文件创建 BufFile。
 * 注意：调用者必须在适当时设置 isInterXact。
 */
static BufFile *
makeBufFile(File firstfile)
{
	BufFile    *file = makeBufFileCommon(1);

	file->files = (File *) palloc(sizeof(File));
	file->files[0] = firstfile;
	file->readOnly = false;
	file->fileset = NULL;
	file->name = NULL;

	return file;
}

/*
 * 再增加一个组件临时文件。
 */
static void
extendBufFile(BufFile *file)
{
	File		pfile;
	ResourceOwner oldowner;

	/* 务必把该文件关联到 BufFile 的资源所有者 */
	oldowner = CurrentResourceOwner;
	CurrentResourceOwner = file->resowner;

	if (file->fileset == NULL)
		pfile = OpenTemporaryFile(file->isInterXact);
	else
		pfile = MakeNewFileSetSegment(file, file->numFiles);

	Assert(pfile >= 0);

	CurrentResourceOwner = oldowner;

	file->files = (File *) repalloc(file->files,
									(file->numFiles + 1) * sizeof(File));
	file->files[file->numFiles] = pfile;
	file->numFiles++;
}

/*
 * 为一个新的临时文件创建 BufFile（若写入超过
 * MAX_PHYSICAL_FILESIZE 字节，它会扩展成
 * 多个临时文件）。
 *
 * 若 interXact 为真，该临时文件不会在
 * 事务结束时自动删除。
 *
 * 注意：若 interXact 为真，调用者最好在能够跨越
 * 事务边界存活的内存上下文里、并带着同样能
 * 存活的资源所有者来调用我们。
 */
BufFile *
BufFileCreateTemp(bool interXact)
{
	BufFile    *file;
	File		pfile;

	/*
	 * 确保为 OpenTemporaryFile 设好临时表空间。
	 * 调用者也许已经做过，但在这里再检查一次似乎有用。
	 * 完全不做的话，临时文件总会被放到默认表空间，
	 * 这是相当难发现的缺陷。若调用者想确保
	 * 任何所需的系统目录访问发生在某个别的
	 * 资源上下文里，
	 * 可以更早去做。
	 */
	PrepareTempTablespaces();

	pfile = OpenTemporaryFile(interXact);
	Assert(pfile >= 0);

	file = makeBufFile(pfile);
	file->isInterXact = interXact;

	return file;
}

/*
 * 为给定 BufFile 的给定段构造名字。
 */
static void
FileSetSegmentName(char *name, const char *buffile_name, int segment)
{
	snprintf(name, MAXPGPATH, "%s.%d", buffile_name, segment);
}

/*
 * 创建支撑基于文件集的 BufFile 的新段文件。
 */
static File
MakeNewFileSetSegment(BufFile *buffile, int segment)
{
	char		name[MAXPGPATH];
	File		file;

	/*
	 * 崩溃重启前可能留下同名文件。
	 * 为了不让 BufFileOpenFileSet() 搞不清有多少段，
	 * 若下一个段号已存在，我们就把它
	 * unlink 掉。
	 */
	FileSetSegmentName(name, buffile->name, segment + 1);
	FileSetDelete(buffile->fileset, name, true);

	/* 创建新段。 */
	FileSetSegmentName(name, buffile->name, segment);
	file = FileSetCreate(buffile->fileset, name);

	/* FileSetCreate 本应已报错退出 */
	Assert(file > 0);

	return file;
}

/*
 * 创建一个 BufFile，其他挂在同一个 SharedFileSet 上、
 * 使用相同名字的后端可以找到它并以只读方式打开。
 *
 * 基于文件集的 BufFiles 命名方案由调用代码决定。
 * 该名字会出现在磁盘上一个或多个文件名里，并可能
 * 向管理员提示是哪个子系统在生成
 * 临时文件数据。由于每个 SharedFileSet 对象由一个或多个
 * 唯一命名的临时目录支撑，名字不会与
 * 无关的 SharedFileSet 对象冲突。
 */
BufFile *
BufFileCreateFileSet(FileSet *fileset, const char *name)
{
	BufFile    *file;

	file = makeBufFileCommon(1);
	file->fileset = fileset;
	file->name = pstrdup(name);
	file->files = (File *) palloc(sizeof(File));
	file->files[0] = MakeNewFileSetSegment(file, 0);
	file->readOnly = false;

	return file;
}

/*
 * 打开先前在另一个后端（或本后端）里用同一 FileSet、
 * 同一名字经 BufFileCreateFileSet 创建的文件。
 * 创建该文件的后端必须已调用 BufFileClose() 或
 * BufFileExportFileSet()，以确保它可供其他
 * 后端打开并把它变成只读。若 missing_ok 为真，表示
 * 可以安全地忽略缺少的文件，则在找不到该名字的 BufFile
 * 时返回 NULL，否则抛出错误。
 */
BufFile *
BufFileOpenFileSet(FileSet *fileset, const char *name, int mode,
				   bool missing_ok)
{
	BufFile    *file;
	char		segment_name[MAXPGPATH];
	Size		capacity = 16;
	File	   *files;
	int			nfiles = 0;

	files = palloc(sizeof(File) * capacity);

	/*
	 * 我们不知道有多少段，因此探测
	 * 文件系统来查明。
	 */
	for (;;)
	{
		/* 看看是否需要扩展文件段数组。 */
		if (nfiles + 1 > capacity)
		{
			capacity *= 2;
			files = repalloc(files, sizeof(File) * capacity);
		}
		/* 尝试载入一个段。 */
		FileSetSegmentName(segment_name, name, nfiles);
		files[nfiles] = FileSetOpen(fileset, segment_name, mode);
		if (files[nfiles] <= 0)
			break;
		++nfiles;

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * 若一个文件都没找到，就不存在这个名字的
	 * BufFile。
	 */
	if (nfiles == 0)
	{
		/* 释放内存 */
		pfree(files);

		if (missing_ok)
			return NULL;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\" from BufFile \"%s\": %m",
						segment_name, name)));
	}

	file = makeBufFileCommon(nfiles);
	file->files = files;
	file->readOnly = (mode == O_RDONLY);
	file->fileset = fileset;
	file->name = pstrdup(name);

	return file;
}

/*
 * 删除在给定 FileSet 里用给定名字经 BufFileCreateFileSet
 * 创建的 BufFile。
 *
 * 不必用本函数显式删除文件。它只是提供一种
 * 主动删除的办法，而不必等到
 * FileSet 被清理。
 *
 * 只有一个后端应当尝试删除给定名字，并且应当知道
 * 它存在且已经导出或关闭，否则应当把 missing_ok
 * 传为真。
 */
void
BufFileDeleteFileSet(FileSet *fileset, const char *name, bool missing_ok)
{
	char		segment_name[MAXPGPATH];
	int			segment = 0;
	bool		found = false;

	/*
	 * 我们不知道该文件有多少段。会一直删
	 * 到没有为止。若连最初的段都找不到，
	 * 就报错。
	 */
	for (;;)
	{
		FileSetSegmentName(segment_name, name, segment);
		if (!FileSetDelete(fileset, segment_name, true))
			break;
		found = true;
		++segment;

		CHECK_FOR_INTERRUPTS();
	}

	if (!found && !missing_ok)
		elog(ERROR, "could not delete unknown BufFile \"%s\"", name);
}

/*
 * BufFileExportFileSet --- 刷出并改为只读，以便共享。
 */
void
BufFileExportFileSet(BufFile *file)
{
	/* 必须是属于某个 FileSet 的文件。 */
	Assert(file->fileset != NULL);

	/* 若有人调用两次，多半是缺陷。 */
	Assert(!file->readOnly);

	BufFileFlush(file);
	file->readOnly = true;
}

/*
 * 关闭 BufFile。
 *
 * 与 fclose() 类似，这也会隐式地 FileCloses 底层文件。
 */
void
BufFileClose(BufFile *file)
{
	int			i;

	/* 刷出尚未写出的数据 */
	BufFileFlush(file);
	/* 关闭并删除底层文件 */
	for (i = 0; i < file->numFiles; i++)
		FileClose(file->files[i]);
	/* 释放缓冲区空间 */
	pfree(file->files);
	pfree(file);
}

/*
 * BufFileLoadBuffer
 *
 * 尽可能从 curOffset 起把一些数据载入缓冲区。
 * 调用时必须 dirty 为假，pos 与 nbytes 为 0。
 * 退出时，nbytes 是已载入的字节数。
 */
static void
BufFileLoadBuffer(BufFile *file)
{
	File		thisfile;
	instr_time	io_start;
	instr_time	io_time;

	/*
	 * 必要时并且可能的话，前进到下一个组件文件。
	 */
	if (file->curOffset >= MAX_PHYSICAL_FILESIZE &&
		file->curFile + 1 < file->numFiles)
	{
		file->curFile++;
		file->curOffset = 0;
	}

	thisfile = file->files[file->curFile];

	if (track_io_timing)
		INSTR_TIME_SET_CURRENT(io_start);
	else
		INSTR_TIME_SET_ZERO(io_start);

	/*
	 * 能读多少就读多少，最多一整缓冲区。
	 */
	file->nbytes = FileRead(thisfile,
							file->buffer.data,
							sizeof(file->buffer.data),
							file->curOffset,
							WAIT_EVENT_BUFFILE_READ);
	if (file->nbytes < 0)
	{
		file->nbytes = 0;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						FilePathName(thisfile))));
	}

	if (track_io_timing)
	{
		INSTR_TIME_SET_CURRENT(io_time);
		INSTR_TIME_ACCUM_DIFF(pgBufferUsage.temp_blk_read_time, io_time, io_start);
	}

	/* 这里我们选择不推进 curOffset */

	if (file->nbytes > 0)
		pgBufferUsage.temp_blks_read++;
}

/*
 * BufFileDumpBuffer
 *
 * 从 curOffset 起转储缓冲区内容。
 * 调用时应当 dirty 为真，nbytes > 0。
 * 退出时，若写成功则清掉 dirty，并推进 curOffset。
 */
static void
BufFileDumpBuffer(BufFile *file)
{
	int			wpos = 0;
	int			bytestowrite;
	File		thisfile;

	/*
	 * 与 BufFileLoadBuffer 不同，即使缓冲区跨越
	 * 组件文件边界，也必须把整个缓冲区转储出去；因此需要循环。
	 */
	while (wpos < file->nbytes)
	{
		off_t		availbytes;
		instr_time	io_start;
		instr_time	io_time;

		/*
		 * 必要时并且可能的话，前进到下一个组件文件。
		 */
		if (file->curOffset >= MAX_PHYSICAL_FILESIZE)
		{
			while (file->curFile + 1 >= file->numFiles)
				extendBufFile(file);
			file->curFile++;
			file->curOffset = 0;
		}

		/*
		 * 确定需要向这个文件写入多少。
		 */
		bytestowrite = file->nbytes - wpos;
		availbytes = MAX_PHYSICAL_FILESIZE - file->curOffset;

		if ((off_t) bytestowrite > availbytes)
			bytestowrite = (int) availbytes;

		thisfile = file->files[file->curFile];

		if (track_io_timing)
			INSTR_TIME_SET_CURRENT(io_start);
		else
			INSTR_TIME_SET_ZERO(io_start);

		bytestowrite = FileWrite(thisfile,
								 file->buffer.data + wpos,
								 bytestowrite,
								 file->curOffset,
								 WAIT_EVENT_BUFFILE_WRITE);
		if (bytestowrite <= 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m",
							FilePathName(thisfile))));

		if (track_io_timing)
		{
			INSTR_TIME_SET_CURRENT(io_time);
			INSTR_TIME_ACCUM_DIFF(pgBufferUsage.temp_blk_write_time, io_time, io_start);
		}

		file->curOffset += bytestowrite;
		wpos += bytestowrite;

		pgBufferUsage.temp_blks_written++;
	}
	file->dirty = false;

	/*
	 * 此时 curOffset 已经推进到缓冲区末尾，
	 * 即原来的值加上 nbytes。我们需要让它指向
	 * 逻辑文件位置，即原来的值加上 pos，以免后者更小
	 * （脏缓冲区里一次小幅向后定位就会这样！）
	 */
	file->curOffset -= (file->nbytes - file->pos);
	if (file->curOffset < 0)	/* 处理可能的跨段 */
	{
		file->curFile--;
		Assert(file->curFile >= 0);
		file->curOffset += MAX_PHYSICAL_FILESIZE;
	}

	/*
	 * 现在可以把缓冲区置空，而不改变逻辑位置
	 */
	file->pos = 0;
	file->nbytes = 0;
}

/*
 * BufFileRead 的几个变体。
 *
 * 与 fread() 类似，只是我们假定元素大小为 1 字节，并通过
 * ereport() 报告 I/O 错误。
 *
 * 若 exact 为真，则读到的字节数不是恰好 size 时也报错
 * （不允许短读）。若 exact 与 eofOK 都为
 * 真，则读到零字节是可以的。
 */
static size_t
BufFileReadCommon(BufFile *file, void *ptr, size_t size, bool exact, bool eofOK)
{
	size_t		start_size = size;
	size_t		nread = 0;
	size_t		nthistime;

	BufFileFlush(file);

	while (size > 0)
	{
		if (file->pos >= file->nbytes)
		{
			/* 尝试把更多数据载入缓冲区。 */
			file->curOffset += file->pos;
			file->pos = 0;
			file->nbytes = 0;
			BufFileLoadBuffer(file);
			if (file->nbytes <= 0)
				break;			/* 没有更多数据可读 */
		}

		nthistime = file->nbytes - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(ptr, file->buffer.data + file->pos, nthistime);

		file->pos += nthistime;
		ptr = (char *) ptr + nthistime;
		size -= nthistime;
		nread += nthistime;
	}

	if (exact &&
		(nread != start_size && !(nread == 0 && eofOK)))
		ereport(ERROR,
				errcode_for_file_access(),
				file->name ?
				errmsg("could not read from file set \"%s\": read only %zu of %zu bytes",
					   file->name, nread, start_size) :
				errmsg("could not read from temporary file: read only %zu of %zu bytes",
					   nread, start_size));

	return nread;
}

/*
 * 旧接口：调用者需要自行检查文件结束或
 * 短读。
 */
size_t
BufFileRead(BufFile *file, void *ptr, size_t size)
{
	return BufFileReadCommon(file, ptr, size, false, false);
}

/*
 * 需要恰好读取指定的大小。
 */
void
BufFileReadExact(BufFile *file, void *ptr, size_t size)
{
	BufFileReadCommon(file, ptr, size, true, false);
}

/*
 * 需要恰好读取指定的大小，但可以选择允许文件结束
 * （此时返回 0）。
 */
size_t
BufFileReadMaybeEOF(BufFile *file, void *ptr, size_t size, bool eofOK)
{
	return BufFileReadCommon(file, ptr, size, true, eofOK);
}

/*
 * BufFileWrite
 *
 * 与 fwrite() 类似，只是我们假定元素大小为 1 字节，并通过
 * ereport() 报告错误。
 */
void
BufFileWrite(BufFile *file, const void *ptr, size_t size)
{
	size_t		nthistime;

	Assert(!file->readOnly);

	while (size > 0)
	{
		if (file->pos >= BLCKSZ)
		{
			/* 缓冲区已满，把它转储出去 */
			if (file->dirty)
				BufFileDumpBuffer(file);
			else
			{
				/* 嗯，从读直接转到写了？ */
				file->curOffset += file->pos;
				file->pos = 0;
				file->nbytes = 0;
			}
		}

		nthistime = BLCKSZ - file->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(file->buffer.data + file->pos, ptr, nthistime);

		file->dirty = true;
		file->pos += nthistime;
		if (file->nbytes < file->pos)
			file->nbytes = file->pos;
		ptr = (const char *) ptr + nthistime;
		size -= nthistime;
	}
}

/*
 * BufFileFlush
 *
 * 与 fflush() 类似，只是 I/O 错误用 ereport() 报告。
 */
static void
BufFileFlush(BufFile *file)
{
	if (file->dirty)
		BufFileDumpBuffer(file);

	Assert(!file->dirty);
}

/*
 * BufFileSeek
 *
 * 与 fseek() 类似，只是当逻辑文件大小超过 off_t
 * 所能表示的最大值时，目标位置需要两个值。
 * 但不支持跨越比这更多的相对定位。
 * I/O 错误由 ereport() 报告。
 *
 * 结果为 0 表示成功，EOF 表示失败。若尝试不可能的定位，
 * 逻辑位置不会移动。
 */
int
BufFileSeek(BufFile *file, int fileno, off_t offset, int whence)
{
	int			newFile;
	off_t		newOffset;

	switch (whence)
	{
		case SEEK_SET:
			if (fileno < 0)
				return EOF;
			newFile = fileno;
			newOffset = offset;
			break;
		case SEEK_CUR:

			/*
			 * 相对定位只考虑有符号偏移，忽略
			 * fileno。注意较大的偏移（> 1GB）在这次
			 * 加法里有溢出风险，除非 off_t 是 64 位。
			 */
			newFile = file->curFile;
			newOffset = (file->curOffset + file->pos) + offset;
			break;
		case SEEK_END:

			/*
			 * 最后一个文件的大小给出该文件的
			 * 结束偏移。
			 */
			newFile = file->numFiles - 1;
			newOffset = FileSize(file->files[file->numFiles - 1]);
			if (newOffset < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not determine size of temporary file \"%s\" from BufFile \"%s\": %m",
								FilePathName(file->files[file->numFiles - 1]),
								file->name)));
			break;
		default:
			elog(ERROR, "invalid whence: %d", whence);
			return EOF;
	}
	while (newOffset < 0)
	{
		if (--newFile < 0)
			return EOF;
		newOffset += MAX_PHYSICAL_FILESIZE;
	}
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/*
		 * 定位点落在现有缓冲区内；只需调整
		 * 缓冲区内的 pos，不必刷出缓冲区。注意无论
		 * 读还是写都可以，但若当时正在写，
		 * 缓冲区仍保持为脏。
		 */
		file->pos = (int) (newOffset - file->curOffset);
		return 0;
	}
	/* 否则必须重新定位缓冲区，因此先刷出任何脏数据 */
	BufFileFlush(file);

	/*
	 * 直到此时才检查是否定位越过了最后一段。上面的
	 * 刷出可能已经创建了新段，因此更早检查
	 * 行不通（至少这段代码不行）。
	 */

	/* 把定位从下一段起点转换成最后一段的末尾 */
	if (newFile == file->numFiles && newOffset == 0)
	{
		newFile--;
		newOffset = MAX_PHYSICAL_FILESIZE;
	}
	while (newOffset > MAX_PHYSICAL_FILESIZE)
	{
		if (++newFile >= file->numFiles)
			return EOF;
		newOffset -= MAX_PHYSICAL_FILESIZE;
	}
	if (newFile >= file->numFiles)
		return EOF;
	/* 定位成功！ */
	file->curFile = newFile;
	file->curOffset = newOffset;
	file->pos = 0;
	file->nbytes = 0;
	return 0;
}

void
BufFileTell(BufFile *file, int *fileno, off_t *offset)
{
	*fileno = file->curFile;
	*offset = file->curOffset + file->pos;
}

/*
 * BufFileSeekBlock --- 按块定位。
 *
 * 绝对定位到文件中第 n 个 BLCKSZ 大小的块的
 * 起点。注意，若文件超过 BLCKSZ * LONG_MAX 字节，
 * 此接口的使用者就会失败，不过那已经相当大；
 * 我们也不处理比这更大的表。
 *
 * 结果为 0 表示成功，EOF 表示失败。若尝试不可能的定位，
 * 逻辑位置不会移动。
 */
int
BufFileSeekBlock(BufFile *file, long blknum)
{
	return BufFileSeek(file,
					   (int) (blknum / BUFFILE_SEG_SIZE),
					   (off_t) (blknum % BUFFILE_SEG_SIZE) * BLCKSZ,
					   SEEK_SET);
}

#ifdef NOT_USED
/*
 * BufFileTellBlock --- 按块报告位置。
 *
 * 忽略当前定位位置里不足一整块的部分。
 */
long
BufFileTellBlock(BufFile *file)
{
	long		blknum;

	blknum = (file->curOffset + file->pos) / BLCKSZ;
	blknum += file->curFile * BUFFILE_SEG_SIZE;
	return blknum;
}

#endif

/*
 * 返回当前基于文件集的 BufFile 大小。
 *
 * 把 BufFileAppend 留下的空洞也计入大小。
 * 失败时调用 ereport()。
 */
int64
BufFileSize(BufFile *file)
{
	int64		lastFileSize;

	Assert(file->fileset != NULL);

	/* 取得最后一个物理文件的大小。 */
	lastFileSize = FileSize(file->files[file->numFiles - 1]);
	if (lastFileSize < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not determine size of temporary file \"%s\" from BufFile \"%s\": %m",
						FilePathName(file->files[file->numFiles - 1]),
						file->name)));

	return ((file->numFiles - 1) * (int64) MAX_PHYSICAL_FILESIZE) +
		lastFileSize;
}

/*
 * 把源文件（由文件集管理）的内容追加到
 * 同一文件集所管理的目标文件末尾。
 *
 * 注意，此操作接过 source 底层资源的所有权。
 * 调用者在此调用之后，不得再对 source 调用 BufFileClose。
 * source 与 target 的资源所有者也必须
 * 一致。
 *
 * 此操作通过调整段文件列表来完成，因此
 * 文件内容总是追加在 MAX_PHYSICAL_FILESIZE 对齐的
 * 边界上，通常会在边界前留下空洞。这些
 * 区域没有任何有用数据，调用者无法
 * 从中读取。
 *
 * 返回目标文件中源内容开始处的块号。
 * 调用者在使用相对于原来 BufFile 空间的块位置时，
 * 应当把这个值当作偏移。
 */
long
BufFileAppend(BufFile *target, BufFile *source)
{
	long		startBlock = target->numFiles * BUFFILE_SEG_SIZE;
	int			newNumFiles = target->numFiles + source->numFiles;
	int			i;

	Assert(target->fileset != NULL);
	Assert(source->readOnly);
	Assert(!source->dirty);
	Assert(source->fileset != NULL);

	if (target->resowner != source->resowner)
		elog(ERROR, "could not append BufFile with non-matching resource owner");

	target->files = (File *)
		repalloc(target->files, sizeof(File) * newNumFiles);
	for (i = target->numFiles; i < newNumFiles; i++)
		target->files[i] = source->files[i - target->numFiles];
	target->numFiles = newNumFiles;

	return startBlock;
}

/*
 * 把 BufFileCreateFileSet 创建的 BufFile 截断到给定的 fileno
 * 和偏移。
 */
void
BufFileTruncateFileSet(BufFile *file, int fileno, off_t offset)
{
	int			numFiles = file->numFiles;
	int			newFile = fileno;
	off_t		newOffset = file->curOffset;
	char		segment_name[MAXPGPATH];
	int			i;

	/*
	 * 遍历直到给定 fileno 的所有文件，删掉
	 * 大于该 fileno 的文件，并把给定文件截断到
	 * 该偏移。注意，若偏移为 0 我们也删掉给定的 fileno，
	 * 只要它不是第一个文件；若是第一个则截断它。
	 */
	for (i = file->numFiles - 1; i >= fileno; i--)
	{
		if ((i != fileno || offset == 0) && i != 0)
		{
			FileSetSegmentName(segment_name, file->name, i);
			FileClose(file->files[i]);
			if (!FileSetDelete(file->fileset, segment_name, true))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not delete fileset \"%s\": %m",
								segment_name)));
			numFiles--;
			newOffset = MAX_PHYSICAL_FILESIZE;

			/*
			 * 这样才能标明我们已经删除了给定的
			 * fileno。
			 */
			if (i == fileno)
				newFile--;
		}
		else
		{
			if (FileTruncate(file->files[i], offset,
							 WAIT_EVENT_BUFFILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(file->files[i]))));
			newOffset = offset;
		}
	}

	file->numFiles = numFiles;

	/*
	 * 若截断点落在现有缓冲区内，只需调整
	 * 缓冲区内的 pos。
	 */
	if (newFile == file->curFile &&
		newOffset >= file->curOffset &&
		newOffset <= file->curOffset + file->nbytes)
	{
		/* 若新位置更大，就不必重置当前位置。 */
		if (newOffset <= file->curOffset + file->pos)
			file->pos = (int) (newOffset - file->curOffset);

		/* 调整当前缓冲区的 nbytes。 */
		file->nbytes = (int) (newOffset - file->curOffset);
	}
	else if (newFile == file->curFile &&
			 newOffset < file->curOffset)
	{
		/*
		 * 截断点在现有文件内、但位于当前位置之前，
		 * 因此可以忘掉当前缓冲区并重置
		 * 当前位置。
		 */
		file->curOffset = newOffset;
		file->pos = 0;
		file->nbytes = 0;
	}
	else if (newFile < file->curFile)
	{
		/*
		 * 截断点在当前文件之前，因此需要相应地重置
		 * 当前位置。
		 */
		file->curFile = newFile;
		file->curOffset = newOffset;
		file->pos = 0;
		file->nbytes = 0;
	}
	/* 若截断点在当前文件之后，则无事可做。 */
}
