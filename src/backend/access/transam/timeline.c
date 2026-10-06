/*-------------------------------------------------------------------------
 *
 * timeline.c
 *		读写时间线历史文件的函数。
 *
 * 时间线历史文件以简单文本格式列出该时间线的
 * 时间线变更。它们与 WAL 段一起归档。
 *
 * 文件命名形如 "<tli>.history"。例如，若数据库
 * 启动并切换到时间线 5，则时间线历史文件将
 * 称为 "00000005.history"。
 *
 * 文件中每一行表示一次时间线切换：
 *
 * <parentTLI> <switchpoint> <reason>
 *
 *	parentTLI	父时间线的 ID
 *	switchpoint 发生切换处 WAL 位置的 XLogRecPtr
 *	reason		说明为何更改时间线的人类可读解释
 *
 * 各字段以制表符分隔。以 # 开头的行为注释，
 * 会被忽略。空行也会被忽略。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/timeline.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/timeline.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "access/xlogdefs.h"
#include "pgstat.h"
#include "storage/fd.h"

/*
 * 将 id 介于 'begin' 与 'end' 之间的所有时间线
 * 历史文件从归档复制到 pg_wal。
 */
void
restoreTimeLineHistoryFiles(TimeLineID begin, TimeLineID end)
{
	char		path[MAXPGPATH];
	char		histfname[MAXFNAMELEN];
	TimeLineID	tli;

	for (tli = begin; tli < end; tli++)
	{
		if (tli == 1)
			continue;

		TLHistoryFileName(histfname, tli);
		if (RestoreArchivedFile(path, histfname, "RECOVERYHISTORY", 0, false))
			KeepFileRestoredFromArchive(path, histfname);
	}
}

/*
 * 尝试读取某时间线的历史文件。
 *
 * 若成功，返回组成 TLIs 列表（给定 TLI，后接其
 * 祖先 TLIs）。若找不到历史文件，则假定该
 * 时间线没有父时间线，并仅返回包含指定时间线
 * ID 的列表。
 */
List *
readTimeLineHistory(TimeLineID targetTLI)
{
	List	   *result;
	char		path[MAXPGPATH];
	char		histfname[MAXFNAMELEN];
	FILE	   *fd;
	TimeLineHistoryEntry *entry;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	bool		fromArchive = false;

	/* 时间线 1 没有历史文件，无需检查 */
	if (targetTLI == 1)
	{
		entry = (TimeLineHistoryEntry *) palloc(sizeof(TimeLineHistoryEntry));
		entry->tli = targetTLI;
		entry->begin = entry->end = InvalidXLogRecPtr;
		return list_make1(entry);
	}

	if (ArchiveRecoveryRequested)
	{
		TLHistoryFileName(histfname, targetTLI);
		fromArchive =
			RestoreArchivedFile(path, histfname, "RECOVERYHISTORY", 0, false);
	}
	else
		TLHistoryFilePath(path, targetTLI);

	fd = AllocateFile(path, "r");
	if (fd == NULL)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		/* 不存在，故假定没有父时间线 */
		entry = (TimeLineHistoryEntry *) palloc(sizeof(TimeLineHistoryEntry));
		entry->tli = targetTLI;
		entry->begin = entry->end = InvalidXLogRecPtr;
		return list_make1(entry);
	}

	result = NIL;

	/*
	 * 解析文件……
	 */
	prevend = InvalidXLogRecPtr;
	for (;;)
	{
		char		fline[MAXPGPATH];
		char	   *res;
		char	   *ptr;
		TimeLineID	tli;
		uint32		switchpoint_hi;
		uint32		switchpoint_lo;
		int			nfields;

		pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_READ);
		res = fgets(fline, sizeof(fline), fd);
		pgstat_report_wait_end();
		if (res == NULL)
		{
			if (ferror(fd))
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m", path)));

			break;
		}

		/* 跳过前导空白并检查 # 注释 */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		nfields = sscanf(fline, "%u\t%X/%X", &tli, &switchpoint_hi, &switchpoint_lo);

		if (nfields < 1)
		{
			/* 期望行的第一个字段为数字形式的时间线 ID */
			ereport(FATAL,
					(errmsg("syntax error in history file: %s", fline),
					 errhint("Expected a numeric timeline ID.")));
		}
		if (nfields != 3)
			ereport(FATAL,
					(errmsg("syntax error in history file: %s", fline),
					 errhint("Expected a write-ahead log switchpoint location.")));

		if (result && tli <= lasttli)
			ereport(FATAL,
					(errmsg("invalid data in history file: %s", fline),
					 errhint("Timeline IDs must be in increasing sequence.")));

		lasttli = tli;

		entry = (TimeLineHistoryEntry *) palloc(sizeof(TimeLineHistoryEntry));
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = ((uint64) (switchpoint_hi)) << 32 | (uint64) switchpoint_lo;
		prevend = entry->end;

		/* 构建列表，最新项在前 */
		result = lcons(entry, result);

		/* 忽略每行的其余部分 */
	}

	FreeFile(fd);

	if (result && targetTLI <= lasttli)
		ereport(FATAL,
				(errmsg("invalid data in history file \"%s\"", path),
				 errhint("Timeline IDs must be less than child timeline's ID.")));

	/*
	 * 再为时间线的"顶端"创建一个条目，历史文件中
	 * 没有该条目。
	 */
	entry = (TimeLineHistoryEntry *) palloc(sizeof(TimeLineHistoryEntry));
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;

	result = lcons(entry, result);

	/*
	 * 若历史文件是从归档取回的，则将其保存在 pg_wal 中
	 * 以备后用。
	 */
	if (fromArchive)
		KeepFileRestoredFromArchive(path, histfname);

	return result;
}

/*
 * 探测给定时间线 ID 是否存在时间线历史文件
 */
bool
existsTimeLineHistory(TimeLineID probeTLI)
{
	char		path[MAXPGPATH];
	char		histfname[MAXFNAMELEN];
	FILE	   *fd;

	/* 时间线 1 没有历史文件，无需检查 */
	if (probeTLI == 1)
		return false;

	if (ArchiveRecoveryRequested)
	{
		TLHistoryFileName(histfname, probeTLI);
		RestoreArchivedFile(path, histfname, "RECOVERYHISTORY", 0, false);
	}
	else
		TLHistoryFilePath(path, probeTLI);

	fd = AllocateFile(path, "r");
	if (fd != NULL)
	{
		FreeFile(fd);
		return true;
	}
	else
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		return false;
	}
}

/*
 * 查找最新存在的时间线，假定 startTLI 存在。
 *
 * 注意：虽有一定启发式成分，但可明确保证
 * (result + 1) 不是已知时间线，因此将
 * 该 ID 赋给新时间线应是安全的。
 */
TimeLineID
findNewestTimeLine(TimeLineID startTLI)
{
	TimeLineID	newestTLI;
	TimeLineID	probeTLI;

	/*
	 * 算法只是探测时间线历史文件是否存在。
	 * XXX 允许序列中有空隙是否有用？
	 */
	newestTLI = startTLI;

	for (probeTLI = startTLI + 1;; probeTLI++)
	{
		if (existsTimeLineHistory(probeTLI))
		{
			newestTLI = probeTLI;	/* probeTLI 存在 */
		}
		else
		{
			/* 不存在，假定已结束 */
			break;
		}
	}

	return newestTLI;
}

/*
 * 创建新的时间线历史文件。
 *
 *	newTLI: 新时间线的 ID
 *	parentTLI: 其直接父时间线的 ID
 *	switchpoint: 系统切换到新时间线时的 WAL 位置
 *	reason: 说明为何切换时间线的人类可读解释
 *
 * 目前仅在恢复结束时使用，因此没有加锁
 * 方面的考虑。但应像 XLogFileInit 一样严谨，以免
 * 落盘一个错误的文件。
 */
void
writeTimeLineHistory(TimeLineID newTLI, TimeLineID parentTLI,
					 XLogRecPtr switchpoint, char *reason)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	char		histfname[MAXFNAMELEN];
	char		buffer[BLCKSZ];
	int			srcfd;
	int			fd;
	int			nbytes;

	Assert(newTLI > parentTLI); /* 否则是错误选定的 newTLI */

	/*
	 * 写入临时文件名。
	 */
	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	/* 此处不要使用 get_sync_bit() —— 只想在填满结束时 fsync */
	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_EXCL);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	/*
	 * 若父时间线存在历史文件，则原样复制
	 */
	if (ArchiveRecoveryRequested)
	{
		TLHistoryFileName(histfname, parentTLI);
		RestoreArchivedFile(path, histfname, "RECOVERYHISTORY", 0, false);
	}
	else
		TLHistoryFilePath(path, parentTLI);

	srcfd = OpenTransientFile(path, O_RDONLY);
	if (srcfd < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", path)));
		/* 不存在，故假定父时间线没有父时间线 */
	}
	else
	{
		for (;;)
		{
			errno = 0;
			pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_READ);
			nbytes = (int) read(srcfd, buffer, sizeof(buffer));
			pgstat_report_wait_end();
			if (nbytes < 0 || errno != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read file \"%s\": %m", path)));
			if (nbytes == 0)
				break;
			errno = 0;
			pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_WRITE);
			if ((int) write(fd, buffer, nbytes) != nbytes)
			{
				int			save_errno = errno;

				/*
				 * 若创建文件失败，则删除它以释放磁盘
				 * 空间
				 */
				unlink(tmppath);

				/*
				 * 若 write 未设置 errno，则假定问题是磁盘空间不足
				 */
				errno = save_errno ? save_errno : ENOSPC;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write to file \"%s\": %m", tmppath)));
			}
			pgstat_report_wait_end();
		}

		if (CloseTransientFile(srcfd) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", path)));
	}

	/*
	 * 追加一行，写明此次时间线分支的细节。
	 *
	 * 若确有父文件，则额外插入一个换行，以防
	 * 父文件末尾没有换行。
	 */
	snprintf(buffer, sizeof(buffer),
			 "%s%u\t%X/%X\t%s\n",
			 (srcfd < 0) ? "" : "\n",
			 parentTLI,
			 LSN_FORMAT_ARGS(switchpoint),
			 reason);

	nbytes = strlen(buffer);
	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_WRITE);
	if ((int) write(fd, buffer, nbytes) != nbytes)
	{
		int			save_errno = errno;

		/*
		 * 若创建文件失败，则删除它以释放磁盘空间
		 */
		unlink(tmppath);
		/* 若 write 未设置 errno，则假定问题是磁盘空间不足 */
		errno = save_errno ? save_errno : ENOSPC;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	/*
	 * 现在将完成的历史文件移到其最终名称处就位。
	 */
	TLHistoryFilePath(path, newTLI);
	Assert(access(path, F_OK) != 0 && errno == ENOENT);
	durable_rename(tmppath, path, ERROR);

	/* 历史文件可以立即归档。 */
	if (XLogArchivingActive())
	{
		TLHistoryFileName(histfname, newTLI);
		XLogArchiveNotify(histfname);
	}
}

/*
 * 为给定时间线与内容写入历史文件。
 *
 * 目前仅在 walreceiver 进程中使用，因此没有
 * 加锁方面的考虑。但应像 XLogFileInit 一样严谨，
 * 以免落盘一个错误的文件。
 */
void
writeTimeLineHistoryFile(TimeLineID tli, char *content, int size)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	int			fd;

	/*
	 * 写入临时文件名。
	 */
	snprintf(tmppath, MAXPGPATH, XLOGDIR "/xlogtemp.%d", (int) getpid());

	unlink(tmppath);

	/* 此处不要使用 get_sync_bit() —— 只想在填满结束时 fsync */
	fd = OpenTransientFile(tmppath, O_RDWR | O_CREAT | O_EXCL);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tmppath)));

	errno = 0;
	pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_FILE_WRITE);
	if ((int) write(fd, content, size) != size)
	{
		int			save_errno = errno;

		/*
		 * 若创建文件失败，则删除它以释放磁盘空间
		 */
		unlink(tmppath);
		/* 若 write 未设置 errno，则假定问题是磁盘空间不足 */
		errno = save_errno ? save_errno : ENOSPC;

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", tmppath)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_TIMELINE_HISTORY_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", tmppath)));
	pgstat_report_wait_end();

	if (CloseTransientFile(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tmppath)));

	/*
	 * 现在将完成的历史文件移到其最终名称处就位，
	 * 替换任何同名的已有文件。
	 */
	TLHistoryFilePath(path, tli);
	durable_rename(tmppath, path, ERROR);
}

/*
 * 若 'expectedTLEs' 包含 id 为 'tli' 的时间线则返回 true
 */
bool
tliInHistory(TimeLineID tli, List *expectedTLEs)
{
	ListCell   *cell;

	foreach(cell, expectedTLEs)
	{
		if (((TimeLineHistoryEntry *) lfirst(cell))->tli == tli)
			return true;
	}

	return false;
}

/*
 * 返回在给定时间线历史中、某一特定时间点
 * 所使用的时间线 ID。
 */
TimeLineID
tliOfPointInHistory(XLogRecPtr ptr, List *history)
{
	ListCell   *cell;

	foreach(cell, history)
	{
		TimeLineHistoryEntry *tle = (TimeLineHistoryEntry *) lfirst(cell);

		if ((XLogRecPtrIsInvalid(tle->begin) || tle->begin <= ptr) &&
			(XLogRecPtrIsInvalid(tle->end) || ptr < tle->end))
		{
			/* 找到了 */
			return tle->tli;
		}
	}

	/* 不应发生。 */
	elog(ERROR, "timeline history was not contiguous");
	return 0;					/* 让编译器安静 */
}

/*
 * 返回我们从给定时间线分支出去的历史点，
 * 以及分支到的时间线（*nextTLI）。若该
 * 时间线为当前时间线（即尚未从它分支出去）则返回 InvalidXLogRecPtr，若
 * 该时间线不属于本服务器历史则抛出错误。
 */
XLogRecPtr
tliSwitchPoint(TimeLineID tli, List *history, TimeLineID *nextTLI)
{
	ListCell   *cell;

	if (nextTLI)
		*nextTLI = 0;
	foreach(cell, history)
	{
		TimeLineHistoryEntry *tle = (TimeLineHistoryEntry *) lfirst(cell);

		if (tle->tli == tli)
			return tle->end;
		if (nextTLI)
			*nextTLI = tle->tli;
	}

	ereport(ERROR,
			(errmsg("requested timeline %u is not in this server's history",
					tli)));
	return InvalidXLogRecPtr;	/* 让编译器安静 */
}
