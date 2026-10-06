/*-------------------------------------------------------------------------
 *
 * xlogarchive.c
 *		归档 WAL 文件以及从归档恢复的函数。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogarchive.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogarchive.h"
#include "common/archive.h"
#include "common/percentrepl.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/startup.h"
#include "postmaster/pgarch.h"
#include "replication/walsender.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"

/*
 * 尝试从离线归档存储中取回指定文件。
 * 若成功，用其完整路径填充 "path"（注意这将是
 * 不遵循常规命名约定的临时文件名），并
 * 返回 true。
 *
 * 若不成功，用常规在线文件的名称填充 "path"
 * （该文件实际可能存在也可能不存在，但我们会尝试使用它），并返回
 * false。
 *
 * 对于固定大小的文件，调用者可传入期望大小，作为
 * 成功恢复的额外交叉检查。若文件大小
 * 未知，则设 expectedSize = 0。
 *
 * 当 'cleanupEnabled' 为 false 时，不要删除归档中的任何
 * 旧 WAL 段。这用于取回初始检查点记录时，
 * 此时我们尚不确定需要回溯多远的 WAL。
 */
bool
RestoreArchivedFile(char *path, const char *xlogfname,
					const char *recovername, off_t expectedSize,
					bool cleanupEnabled)
{
	char		xlogpath[MAXPGPATH];
	char	   *xlogRestoreCmd;
	char		lastRestartPointFname[MAXPGPATH];
	int			rc;
	struct stat stat_buf;
	XLogSegNo	restartSegNo;
	XLogRecPtr	restartRedoPtr;
	TimeLineID	restartTli;

	/*
	 * 非归档恢复时忽略 restore_command（意味着我们处于
	 * 崩溃恢复）。
	 */
	if (!ArchiveRecoveryRequested)
		goto not_available;

	/* 备库模式下可能未提供 restore_command */
	if (recoveryRestoreCommand == NULL || strcmp(recoveryRestoreCommand, "") == 0)
		goto not_available;

	/*
	 * 进行归档恢复时，我们总是优先使用已归档的日志文件，即使
	 * XLOGDIR 中存在同名文件。原因是 XLOGDIR 中的文件
	 * 可能是在备份 $PGDATA 时被复制并恢复的旧的、未填满
	 * 或部分填满的版本。
	 *
	 * 我们本可通过比较本地副本与归档副本的
	 * lastchange 时间戳略作优化，但没有 API 可
	 * 做此事，也无法保证复制到归档时
	 * lastchange 时间戳被正确保留。我们的目标是稳健性，
	 * 因此选择不做。
	 *
	 * 不过，若无法从归档获得日志文件，我们会尝试
	 * 使用存在的 XLOGDIR 文件。这样可以利用
	 * 尚未传送到归档的日志段。
	 *
	 * 注意，从归档复制回来时我们并不实际覆盖任何
	 * 文件，因为 restore_command 可能无意中恢复
	 * 不合适的 xlog，或者它们可能已损坏，因此我们可能希望稍后回退
	 * 到当前 XLOGDIR 中剩余的段。从
	 * 归档复制的文件名始终相同，确保在长时间
	 * 恢复中不会耗尽磁盘空间。
	 */
	snprintf(xlogpath, MAXPGPATH, XLOGDIR "/%s", recovername);

	/*
	 * 确保不存在名为 recovername 的现有文件。
	 */
	if (stat(xlogpath, &stat_buf) != 0)
	{
		if (errno != ENOENT)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							xlogpath)));
	}
	else
	{
		if (unlink(xlogpath) != 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							xlogpath)));
	}

	/*
	 * 计算日志传送复制期间使用的归档文件截断点。
	 * 早于此点的所有文件都可从归档中
	 * 删除，尽管并无必须这样做的要求。
	 *
	 * 若未启用清理，则用
	 * InvalidXLogRecPtr 的文件名初始化，这将因 WAL
	 * 文件名的字母排序属性而阻止从归档删除
	 * 任何 WAL 文件。
	 *
	 * 一旦我们成功定位到启动恢复所用检查点的
	 * redo 指针，就绝不再请求早于上次
	 * restartpoint 的 redo 指针之前的文件。当 redo 开始时我们知道已经
	 * 成功定位到它，因此无需额外状态
	 * 标志来表示何时可以开始从归档删除
	 * WAL 文件。
	 */
	if (cleanupEnabled)
	{
		GetOldestRestartPoint(&restartRedoPtr, &restartTli);
		XLByteToSeg(restartRedoPtr, restartSegNo, wal_segment_size);
		XLogFileName(lastRestartPointFname, restartTli, restartSegNo,
					 wal_segment_size);
		/* 我们不应需要早于上次 restart point 的任何内容 */
		Assert(strcmp(lastRestartPointFname, xlogfname) <= 0);
	}
	else
		XLogFileName(lastRestartPointFname, 0, 0, wal_segment_size);

	/* 构造要执行的恢复命令 */
	xlogRestoreCmd = BuildRestoreCommand(recoveryRestoreCommand,
										 xlogpath, xlogfname,
										 lastRestartPointFname);

	ereport(DEBUG3,
			(errmsg_internal("executing restore command \"%s\"",
							 xlogRestoreCmd)));

	fflush(NULL);
	pgstat_report_wait_start(WAIT_EVENT_RESTORE_COMMAND);

	/*
	 * PreRestoreCommand() 告知启动进程的 SIGTERM 处理程序
	 * 应立即 proc_exit()。这是在
	 * system() 调用期间完成的，因为在其执行期间没有好的办法中断。
	 * 由于我们可能在信号处理程序中调用
	 * proc_exit()，最好把任何额外逻辑放在
	 * PreRestoreCommand()/PostRestoreCommand() 区段之前或之后。
	 */
	PreRestoreCommand();

	/*
	 * 把 xlog 从归档存储复制到 XLOGDIR
	 */
	rc = system(xlogRestoreCmd);

	PostRestoreCommand();

	pgstat_report_wait_end();
	pfree(xlogRestoreCmd);

	if (rc == 0)
	{
		/*
		 * 命令看似成功，但还要确认文件现在
		 * 确实存在且大小正确。
		 */
		if (stat(xlogpath, &stat_buf) == 0)
		{
			if (expectedSize > 0 && stat_buf.st_size != expectedSize)
			{
				int			elevel;

				/*
				 * 若在备库模式下发现部分文件，我们假定这是
				 * 因为它正在被复制到归档，并继续
				 * 尝试。
				 *
				 * 否则把大小错误的文件当作 FATAL，以确保
				 * DBA 会注意到，但这是否过强？我们本可尝试
				 * 用该文件的本地副本继续前进……但
				 * 问题是很可能没有本地副本，我们会
				 * 错误地断定已到达 WAL 末尾且
				 * 恢复已完成……
				 */
				if (StandbyMode && stat_buf.st_size < expectedSize)
					elevel = DEBUG1;
				else
					elevel = FATAL;
				ereport(elevel,
						(errmsg("archive file \"%s\" has wrong size: %lld instead of %lld",
								xlogfname,
								(long long int) stat_buf.st_size,
								(long long int) expectedSize)));
				return false;
			}
			else
			{
				ereport(LOG,
						(errmsg("restored log file \"%s\" from archive",
								xlogfname)));
				strcpy(path, xlogpath);
				return true;
			}
		}
		else
		{
			/* stat 失败 */
			int			elevel = (errno == ENOENT) ? LOG : FATAL;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", xlogpath),
					 errdetail("restore_command returned a zero exit status, but stat() failed.")));
		}
	}

	/*
	 * 记住，我们向前滚动 UNTIL 恢复失败，因此此处的失败
	 * 只是过程的一部分……这使得难以判断
	 * 恢复失败是因为没有可恢复的归档，
	 * 还是因为管理员错误地指定了恢复
	 * 程序。我们不得不假定是前者。
	 *
	 * 不过，若失败是由任何信号导致的，最好
	 * 放弃并中止恢复。（若此处 "return false"，上层将
	 * 假定恢复完成并启动数据库！）在子进程
	 * SIGINT 与 SIGQUIT 上中止至关重要，因为按规范
	 * system() 在等待时忽略 SIGINT 与 SIGQUIT；若我们看到其中
	 * 之一，很大概率我们也本应收到它。
	 *
	 * 对 SIGTERM，假定我们收到了快速关闭请求，并干净地
	 * 退出。是我们先收到 SIGTERM，还是
	 * 子进程先收到，纯属运气。若我们先收到，信号处理程序会调用
	 * proc_exit，否则我们在此处调用。若我们或子进程因
	 * 快速关闭请求以外的任何原因收到
	 * SIGTERM，postmaster 看到我们意外退出时
	 * 将执行立即关闭。
	 *
	 * 我们也把诸如 "command not found" 这类硬性 shell 错误视为致命。
	 */
	if (wait_result_is_signal(rc, SIGTERM))
		proc_exit(1);

	ereport(wait_result_is_any_signal(rc, true) ? FATAL : DEBUG2,
			(errmsg("could not restore file \"%s\" from archive: %s",
					xlogfname, wait_result_to_str(rc))));

not_available:

	/*
	 * 若归档文件不可用，XLOGDIR 中可能仍有该
	 * 文件的某个版本，因此把它作为要打开的文件名返回。
	 *
	 * 在许多恢复场景中我们也预期这会失败，但若如此，那
	 * 只意味着我们已到达 WAL 末尾。
	 */
	snprintf(path, MAXPGPATH, XLOGDIR "/%s", xlogfname);
	return false;
}

/*
 * 尝试在恢复期间执行外部 shell 命令。
 *
 * 'command' 是要执行的 shell 命令，'commandName' 是
 * 日志中发出的描述该命令的人类可读名称。若
 * 'failOnSignal' 为真且命令被信号杀死，则抛出 FATAL
 * 错误。否则发出 WARNING。
 *
 * 当前用于 recovery_end_command 与 archive_cleanup_command。
 */
void
ExecuteRecoveryCommand(const char *command, const char *commandName,
					   bool failOnSignal, uint32 wait_event_info)
{
	char	   *xlogRecoveryCmd;
	char		lastRestartPointFname[MAXPGPATH];
	int			rc;
	XLogSegNo	restartSegNo;
	XLogRecPtr	restartRedoPtr;
	TimeLineID	restartTli;

	Assert(command && commandName);

	/*
	 * 计算日志传送复制期间使用的归档文件截断点。
	 * 早于此点的所有文件都可从归档中删除，
	 * 尽管并无必须这样做的要求。
	 */
	GetOldestRestartPoint(&restartRedoPtr, &restartTli);
	XLByteToSeg(restartRedoPtr, restartSegNo, wal_segment_size);
	XLogFileName(lastRestartPointFname, restartTli, restartSegNo,
				 wal_segment_size);

	/*
	 * 构造要执行的命令
	 */
	xlogRecoveryCmd = replace_percent_placeholders(command, commandName, "r", lastRestartPointFname);

	ereport(DEBUG3,
			(errmsg_internal("executing %s \"%s\"", commandName, command)));

	/*
	 * 执行所构造的命令
	 */
	fflush(NULL);
	pgstat_report_wait_start(wait_event_info);
	rc = system(xlogRecoveryCmd);
	pgstat_report_wait_end();

	pfree(xlogRecoveryCmd);

	if (rc != 0)
	{
		/*
		 * 若失败是由任何信号导致的，最好放弃并
		 * 中止恢复。见 RestoreArchivedFile() 中的注释。
		 */
		ereport((failOnSignal && wait_result_is_any_signal(rc, true)) ? FATAL : WARNING,
		/*------
		   translator: First %s represents a postgresql.conf parameter name like
		  "recovery_end_command", the 2nd is the value of that parameter, the
		  third an already translated error message. */
				(errmsg("%s \"%s\": %s", commandName,
						command, wait_result_to_str(rc))));
	}
}


/*
 * 文件已从归档恢复到临时文件名（path）下，
 * 现在我们想保留它。将其重命名为 pg_wal 中的永久
 * 文件名（xlogfname），替换任何同名的现有文件。
 */
void
KeepFileRestoredFromArchive(const char *path, const char *xlogfname)
{
	char		xlogfpath[MAXPGPATH];
	bool		reload = false;
	struct stat statbuf;

	snprintf(xlogfpath, MAXPGPATH, XLOGDIR "/%s", xlogfname);

	if (stat(xlogfpath, &statbuf) == 0)
	{
		char		oldpath[MAXPGPATH];

#ifdef WIN32
		static unsigned int deletedcounter = 1;

		/*
		 * 在 Windows 上，若另一进程（例如 walsender 进程）以
		 * FILE_SHARE_DELETE 模式打开着该文件，unlink 会成功，但该
		 * 文件在目录列表中仍会显示，直到最后一个句柄
		 * 关闭，并且在此之前我们无法把新文件重命名到其位置。
		 * 为避免该问题，先把旧文件重命名为临时
		 * 名。用计数器创建唯一文件名，因为同一
		 * 文件可能多次从归档恢复，且
		 * walsender 仍可能持有其已删除的旧版本。
		 */
		snprintf(oldpath, MAXPGPATH, "%s.deleted%u",
				 xlogfpath, deletedcounter++);
		if (rename(xlogfpath, oldpath) != 0)
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rename file \"%s\" to \"%s\": %m",
							xlogfpath, oldpath)));
		}
#else
		/* 缓冲区大小相同，因此绝不会截断 */
		strlcpy(oldpath, xlogfpath, MAXPGPATH);
#endif
		if (unlink(oldpath) != 0)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							xlogfpath)));
		reload = true;
	}

	durable_rename(path, xlogfpath, ERROR);

	/*
	 * 强制创建 .done 文件，以防恢复出的段稍后
	 * 再次被归档。
	 */
	if (XLogArchiveMode != ARCHIVE_MODE_ALWAYS)
		XLogArchiveForceDone(xlogfname);
	else
		XLogArchiveNotify(xlogfname);

	/*
	 * 若替换了现有文件，由于 walsender 可能正打开着它，
	 * 请求它们重新加载当前打开的段。这仅对
	 * WAL 段需要，walsender 不会保持其他文件打开，但
	 * 过于频繁地这样做并无害处，且我们不知道这里处理的是
	 * 何种文件。
	 */
	if (reload)
		WalSndRqstFileReload();

	/*
	 * 向 walsender 发信号表示有新 WAL 到达。同样，若我们恢复的
	 * 不是 WAL 段，这也不是必要的，但这样做
	 * 也无害。
	 */
	WalSndWakeup(true, false);
}

/*
 * XLogArchiveNotify
 *
 * 创建归档通知文件
 *
 * 通知文件的名称就是归档进程将拾取的消息，
 * 例如我们写入 0000000100000001000000C6.ready，
 * 然后归档进程知道要归档 XLOGDIR/0000000100000001000000C6，
 * 完成后将其重命名为 0000000100000001000000C6.done
 */
void
XLogArchiveNotify(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];
	FILE	   *fd;

	/* 插入一个名为 <XLOG>.ready 的空文件 */
	StatusFilePath(archiveStatusPath, xlog, ".ready");
	fd = AllocateFile(archiveStatusPath, "w");
	if (fd == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create archive status file \"%s\": %m",
						archiveStatusPath)));
		return;
	}
	if (FreeFile(fd))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write archive status file \"%s\": %m",
						archiveStatusPath)));
		return;
	}

	/*
	 * 时间线历史文件给予最高归档优先级，以降低
	 * 已提升备库选择已在使用的时间线的
	 * 机会。不过，归档进程通常会尝试在每次扫描
	 * archive_status 目录时收集多个待归档文件，
	 * 这意味着新创建的时间线历史文件可能
	 * 会有一段时间未被归档。为确保归档进程尽快
	 * 拾取时间线历史文件，我们强制归档进程在
	 * 下次查找待归档文件时扫描
	 * archive_status 目录。
	 */
	if (IsTLHistoryFileName(xlog))
		PgArchForceDirScan();

	/* 通知归档进程有事可做 */
	if (IsUnderPostmaster)
		PgArchWakeup();
}

/*
 * 使用文件名的段号表示形式进行通知的便捷例程
 */
void
XLogArchiveNotifySeg(XLogSegNo segno, TimeLineID tli)
{
	char		xlog[MAXFNAMELEN];

	Assert(tli != 0);

	XLogFileName(xlog, tli, segno, wal_segment_size);
	XLogArchiveNotify(xlog);
}

/*
 * XLogArchiveForceDone
 *
 * 强制发出 XLOG 段文件已成功归档的通知，
 * 方法是创建 <XLOG>.done，无论 <XLOG>.ready
 * 是否存在。
 */
void
XLogArchiveForceDone(const char *xlog)
{
	char		archiveReady[MAXPGPATH];
	char		archiveDone[MAXPGPATH];
	struct stat stat_buf;
	FILE	   *fd;

	/* 若已知已完成则退出 */
	StatusFilePath(archiveDone, xlog, ".done");
	if (stat(archiveDone, &stat_buf) == 0)
		return;

	/* 若 .ready 存在，将其重命名为 .done */
	StatusFilePath(archiveReady, xlog, ".ready");
	if (stat(archiveReady, &stat_buf) == 0)
	{
		(void) durable_rename(archiveReady, archiveDone, WARNING);
		return;
	}

	/* 插入一个名为 <XLOG>.done 的空文件 */
	fd = AllocateFile(archiveDone, "w");
	if (fd == NULL)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not create archive status file \"%s\": %m",
						archiveDone)));
		return;
	}
	if (FreeFile(fd))
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not write archive status file \"%s\": %m",
						archiveDone)));
		return;
	}
}

/*
 * XLogArchiveCheckDone
 *
 * 当我们准备删除或回收旧的 XLOG 段
 * 文件或备份历史文件时调用。若可以删除则返回 true。
 * 若尚未到删除时机，确保存在 .ready 文件，并返回
 * false。
 *
 * 若 <XLOG>.done 存在，则返回 true；否则若 <XLOG>.ready 存在，
 * 则返回 false；否则创建 <XLOG>.ready 并返回 false。
 *
 * 我们这样做的原因是：若最初尝试创建
 * <XLOG>.ready 失败，会在后续检查点期间重试。
 */
bool
XLogArchiveCheckDone(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];
	struct stat stat_buf;

	/* 若 archive_mode 为 "off"，该文件总是可删除。 */
	if (!XLogArchivingActive())
		return true;

	/*
	 * 归档恢复期间，若 archive_mode 不是
	 * "always"，该文件可删除。
	 */
	if (!XLogArchivingAlways() &&
		GetRecoveryState() == RECOVERY_STATE_ARCHIVE)
		return true;

	/*
	 * 在此逻辑点，注意我们要么是 archive_mode 设为
	 * "on" 或 "always" 的主库，要么是 archive_mode
	 * 设为 "always" 的备库。
	 */

	/* 先检查 .done --- 这表示归档进程已处理完它 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	/* 检查 .ready --- 这表示归档进程仍在处理它 */
	StatusFilePath(archiveStatusPath, xlog, ".ready");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return false;

	/* 竞态 --- 也许归档进程刚完成，因此再检查 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	/* 重试创建 .ready 文件 */
	XLogArchiveNotify(xlog);
	return false;
}

/*
 * XLogArchiveIsBusy
 *
 * 检查 XLOG 段文件是否仍未归档。
 * 这几乎但并不完全是 XLogArchiveCheckDone 的逆：首先
 * 我们无权重新创建 .ready 文件，其次
 * 若文件已经不在则应视为不忙。（需要此检查
 * 以处理检查点已删除不再需要的文件的
 * 竞态条件。）
 */
bool
XLogArchiveIsBusy(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];
	struct stat stat_buf;

	/* 先检查 .done --- 这表示归档进程已处理完它 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return false;

	/* 检查 .ready --- 这表示归档进程仍在处理它 */
	StatusFilePath(archiveStatusPath, xlog, ".ready");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	/* 竞态 --- 也许归档进程刚完成，因此再检查 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return false;

	/*
	 * 检查 WAL 文件是否已被检查点移除，这
	 * 意味着它已经被归档，并解释了为何看不到
	 * 其状态文件。
	 */
	snprintf(archiveStatusPath, MAXPGPATH, XLOGDIR "/%s", xlog);
	if (stat(archiveStatusPath, &stat_buf) != 0 &&
		errno == ENOENT)
		return false;

	return true;
}

/*
 * XLogArchiveIsReadyOrDone
 *
 * 检查 XLOG 段文件是否有 .ready 或 .done 文件。
 * 这与 XLogArchiveIsBusy() 类似，但若文件
 * 已归档或即将被归档则返回 true。
 *
 * 当前仅在恢复时使用。在正常运行期间这样检查
 * 会有竞态：我们检查时或刚返回后，文件可能被移除或
 * 标上 .ready。
 */
bool
XLogArchiveIsReadyOrDone(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];
	struct stat stat_buf;

	/* 先检查 .done --- 这表示归档进程已处理完它 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	/* 检查 .ready --- 这表示归档进程仍在处理它 */
	StatusFilePath(archiveStatusPath, xlog, ".ready");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	/* 竞态 --- 也许归档进程刚完成，因此再检查 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * XLogArchiveIsReady
 *
 * 检查 XLOG 段文件是否有归档通知（.ready）
 * 文件。
 */
bool
XLogArchiveIsReady(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];
	struct stat stat_buf;

	StatusFilePath(archiveStatusPath, xlog, ".ready");
	if (stat(archiveStatusPath, &stat_buf) == 0)
		return true;

	return false;
}

/*
 * XLogArchiveCleanup
 *
 * 清理特定 xlog 段的归档通知 file(s)
 */
void
XLogArchiveCleanup(const char *xlog)
{
	char		archiveStatusPath[MAXPGPATH];

	/* 移除 .done 文件 */
	StatusFilePath(archiveStatusPath, xlog, ".done");
	unlink(archiveStatusPath);
	/* 失败时是否应抱怨？ */

	/* 若存在则移除 .ready 文件 --- 正常情况下不应存在 */
	StatusFilePath(archiveStatusPath, xlog, ".ready");
	unlink(archiveStatusPath);
	/* 失败时是否应抱怨？ */
}
