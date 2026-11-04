/*-------------------------------------------------------------------------
 *
 * parsexlog.c
 *	  Functions for reading Write-Ahead-Log
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "access/rmgr.h"
#include "access/xact.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "fe_utils/archive.h"
#include "filemap.h"
#include "pg_rewind.h"

/*
 * RmgrNames 是内置资源管理器名称的数组，使错误
 * 消息更友好一些。
                                                              */
#define PG_RMGR(symname,name,redo,desc,identify,startup,cleanup,mask,decode) \
  name,

static const char *RmgrNames[RM_MAX_ID + 1] = {
#include "access/rmgrlist.h"
};

#define RmgrName(rmid) (((rmid) <= RM_MAX_BUILTIN_ID) ? \
						RmgrNames[rmid] : "custom")

static void extractPageInfo(XLogReaderState *record);

static int	xlogreadfd = -1;
static XLogSegNo xlogreadsegno = 0;
static char xlogfpath[MAXPGPATH];

typedef struct XLogPageReadPrivate
{
	const char *restoreCommand;
	int			tliIndex;
} XLogPageReadPrivate;

static int	SimpleXLogPageRead(XLogReaderState *xlogreader,
							   XLogRecPtr targetPagePtr,
							   int reqLen, XLogRecPtr targetRecPtr, char *readBuf);

/*
 * 从 datadir/pg_wal 读取 WAL，从目标时间线历史中             
 * 时间线索引 'tliIndex' 的 'startpoint' 开始，直到'endpoint'。   
 * 记录 WAL 记录触及的数据块，并在页面映射中返回。      
 *
 * 'endpoint' 是要读取的最后一条记录的末尾。起始于       
 * 'endpoint' 的记录为首条不读取。    
 */
void
extractPageMap(const char *datadir, XLogRecPtr startpoint, int tliIndex,
			   XLogRecPtr endpoint, const char *restoreCommand)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;
	XLogPageReadPrivate private;

	private.tliIndex = tliIndex;
	private.restoreCommand = restoreCommand;
	xlogreader = XLogReaderAllocate(WalSegSz, datadir,
									XL_ROUTINE(.page_read = &SimpleXLogPageRead),
									&private);
	if (xlogreader == NULL)
		pg_fatal("out of memory while allocating a WAL reading processor");

	XLogBeginRead(xlogreader, startpoint);
	do
	{
		record = XLogReadRecord(xlogreader, &errormsg);

		if (record == NULL)
		{
			XLogRecPtr	errptr = xlogreader->EndRecPtr;

			if (errormsg)
				pg_fatal("could not read WAL record at %X/%X: %s",
						 LSN_FORMAT_ARGS(errptr),
						 errormsg);
			else
				pg_fatal("could not read WAL record at %X/%X",
						 LSN_FORMAT_ARGS(errptr));
		}

		extractPageInfo(xlogreader);
	} while (xlogreader->EndRecPtr < endpoint);

	/*
	 * 如果 'endpoint' 未精确指向记录边界，说明调用者
	 * 搞错了。  
	 */
	if (xlogreader->EndRecPtr != endpoint)
		pg_fatal("end pointer %X/%X is not a valid end point; expected %X/%X",
				 LSN_FORMAT_ARGS(endpoint), LSN_FORMAT_ARGS(xlogreader->EndRecPtr));

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}
}

/*
 * 读取一条 WAL 记录。返回记录的结束位置，不对
 * 记录本身做任何处理。
                                                                          */
XLogRecPtr
readOneRecord(const char *datadir, XLogRecPtr ptr, int tliIndex,
			  const char *restoreCommand)
{
	XLogRecord *record;
	XLogReaderState *xlogreader;
	char	   *errormsg;
	XLogPageReadPrivate private;
	XLogRecPtr	endptr;

	private.tliIndex = tliIndex;
	private.restoreCommand = restoreCommand;
	xlogreader = XLogReaderAllocate(WalSegSz, datadir,
									XL_ROUTINE(.page_read = &SimpleXLogPageRead),
									&private);
	if (xlogreader == NULL)
		pg_fatal("out of memory while allocating a WAL reading processor");

	XLogBeginRead(xlogreader, ptr);
	record = XLogReadRecord(xlogreader, &errormsg);
	if (record == NULL)
	{
		if (errormsg)
			pg_fatal("could not read WAL record at %X/%X: %s",
					 LSN_FORMAT_ARGS(ptr), errormsg);
		else
			pg_fatal("could not read WAL record at %X/%X",
					 LSN_FORMAT_ARGS(ptr));
	}
	endptr = xlogreader->EndRecPtr;

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}

	return endptr;
}

/*
 * 查找给定 WAL 位置之前的上一个检查点。
                                      */
void
findLastCheckpoint(const char *datadir, XLogRecPtr forkptr, int tliIndex,
				   XLogRecPtr *lastchkptrec, TimeLineID *lastchkpttli,
				   XLogRecPtr *lastchkptredo, const char *restoreCommand)
{
	/* 向后遍历，从给定记录开始                                   */
	XLogRecord *record;
	XLogRecPtr	searchptr;
	XLogReaderState *xlogreader;
	char	   *errormsg;
	XLogPageReadPrivate private;
	XLogSegNo	current_segno = 0;
	TimeLineID	current_tli = 0;

	/*
	 * 给定的分叉指针指向最后一条公共记录的末尾，
	 * 如果上一条记录恰好在页面边界结束，它不一定是
	 * 下一条记录的开头。这种情况下跳过页面头，
	 * 以找到下一条记录。                             
	 */
	if (forkptr % XLOG_BLCKSZ == 0)
	{
		if (XLogSegmentOffset(forkptr, WalSegSz) == 0)
			forkptr += SizeOfXLogLongPHD;
		else
			forkptr += SizeOfXLogShortPHD;
	}

	private.tliIndex = tliIndex;
	private.restoreCommand = restoreCommand;
	xlogreader = XLogReaderAllocate(WalSegSz, datadir,
									XL_ROUTINE(.page_read = &SimpleXLogPageRead),
									&private);
	if (xlogreader == NULL)
		pg_fatal("out of memory while allocating a WAL reading processor");

	searchptr = forkptr;
	for (;;)
	{
		uint8		info;

		XLogBeginRead(xlogreader, searchptr);
		record = XLogReadRecord(xlogreader, &errormsg);

		if (record == NULL)
		{
			if (errormsg)
				pg_fatal("could not find previous WAL record at %X/%X: %s",
						 LSN_FORMAT_ARGS(searchptr),
						 errormsg);
			else
				pg_fatal("could not find previous WAL record at %X/%X",
						 LSN_FORMAT_ARGS(searchptr));
		}

		/* 检测是否打开了新的 WAL 文件                         */
		if (xlogreader->seg.ws_tli != current_tli ||
			xlogreader->seg.ws_segno != current_segno)
		{
			char		xlogfname[MAXFNAMELEN];

			snprintf(xlogfname, MAXFNAMELEN, XLOGDIR "/");

			/* 更新当前值                */
			current_tli = xlogreader->seg.ws_tli;
			current_segno = xlogreader->seg.ws_segno;

			XLogFileName(xlogfname + sizeof(XLOGDIR),
						 current_tli, current_segno, WalSegSz);

			/* 将此文件标记为不删除的文件                            */
			keepwal_add_entry(xlogfname);
		}

		/*
		 * 检查是否是检查点记录。该检查点记录必须是
		 * WAL 分叉前最新的检查点，而不是主库为倒回
		 * 而停止时的检查点。                                    
		 */
		info = XLogRecGetInfo(xlogreader) & ~XLR_INFO_MASK;
		if (searchptr < forkptr &&
			XLogRecGetRmid(xlogreader) == RM_XLOG_ID &&
			(info == XLOG_CHECKPOINT_SHUTDOWN ||
			 info == XLOG_CHECKPOINT_ONLINE))
		{
			CheckPoint	checkPoint;

			memcpy(&checkPoint, XLogRecGetData(xlogreader), sizeof(CheckPoint));
			*lastchkptrec = searchptr;
			*lastchkpttli = checkPoint.ThisTimeLineID;
			*lastchkptredo = checkPoint.redo;
			break;
		}

		/* 向后遍历到上一条记录。                        */
		searchptr = record->xl_prev;
	}

	XLogReaderFree(xlogreader);
	if (xlogreadfd != -1)
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}
}

/* XLogReader 回调函数，用于读取 WAL 页                       */
static int
SimpleXLogPageRead(XLogReaderState *xlogreader, XLogRecPtr targetPagePtr,
				   int reqLen, XLogRecPtr targetRecPtr, char *readBuf)
{
	XLogPageReadPrivate *private = (XLogPageReadPrivate *) xlogreader->private_data;
	uint32		targetPageOff;
	XLogRecPtr	targetSegEnd;
	XLogSegNo	targetSegNo;
	int			r;

	XLByteToSeg(targetPagePtr, targetSegNo, WalSegSz);
	XLogSegNoOffsetToRecPtr(targetSegNo + 1, 0, WalSegSz, targetSegEnd);
	targetPageOff = XLogSegmentOffset(targetPagePtr, WalSegSz);

	/*
	 * 检查是否需要切换到新段，因为请求的记录不在
	 * 当前打开的段中。                
	 */
	if (xlogreadfd >= 0 &&
		!XLByteInSeg(targetPagePtr, xlogreadsegno, WalSegSz))
	{
		close(xlogreadfd);
		xlogreadfd = -1;
	}

	XLByteToSeg(targetPagePtr, xlogreadsegno, WalSegSz);

	if (xlogreadfd < 0)
	{
		char		xlogfname[MAXFNAMELEN];

		/*
		 * 由于不完整段被复制到后续时间线中，切换到
		 * 包含所需段的时间线。考虑到此扫描既可正向
		 * 也可反向进行，同时也考虑相应地
		 * 切换时间线。                            
		 */
		while (private->tliIndex < targetNentries - 1 &&
			   targetHistory[private->tliIndex].end < targetSegEnd)
			private->tliIndex++;
		while (private->tliIndex > 0 &&
			   targetHistory[private->tliIndex].begin >= targetSegEnd)
			private->tliIndex--;

		XLogFileName(xlogfname, targetHistory[private->tliIndex].tli,
					 xlogreadsegno, WalSegSz);

		snprintf(xlogfpath, MAXPGPATH, "%s/" XLOGDIR "/%s",
				 xlogreader->segcxt.ws_dir, xlogfname);

		xlogreadfd = open(xlogfpath, O_RDONLY | PG_BINARY, 0);

		if (xlogreadfd < 0)
		{
			/*
			 * 若没有 restore_command 可执行，则退出。  
			 */
			if (private->restoreCommand == NULL)
			{
				pg_log_error("could not open file \"%s\": %m", xlogfpath);
				return -1;
			}

			/*
			 * 由于有 restore_command，尝试从归档中
			 * 获取缺失的 WAL 文件。          
			 */
			xlogreadfd = RestoreArchivedFile(xlogreader->segcxt.ws_dir,
											 xlogfname,
											 WalSegSz,
											 private->restoreCommand);

			if (xlogreadfd < 0)
				return -1;
			else
				pg_log_debug("using file \"%s\" restored from archive",
							 xlogfpath);
		}
	}

	/*
	 * 此时，正确的段已打开。             
	 */
	Assert(xlogreadfd != -1);

	/* 读取请求的页面                 */
	if (lseek(xlogreadfd, (off_t) targetPageOff, SEEK_SET) < 0)
	{
		pg_log_error("could not seek in file \"%s\": %m", xlogfpath);
		return -1;
	}


	r = read(xlogreadfd, readBuf, XLOG_BLCKSZ);
	if (r != XLOG_BLCKSZ)
	{
		if (r < 0)
			pg_log_error("could not read file \"%s\": %m", xlogfpath);
		else
			pg_log_error("could not read file \"%s\": read %d of %zu",
						 xlogfpath, r, (Size) XLOG_BLCKSZ);

		return -1;
	}

	Assert(targetSegNo == xlogreadsegno);

	xlogreader->seg.ws_tli = targetHistory[private->tliIndex].tli;
	return XLOG_BLCKSZ;
}

/*
 * 提取当前记录修改了哪些块的信息。
                                                 */
static void
extractPageInfo(XLogReaderState *record)
{
	int			block_id;
	RmgrId		rmid = XLogRecGetRmid(record);
	uint8		info = XLogRecGetInfo(record);
	uint8		rminfo = info & ~XLR_INFO_MASK;

	/* 这是我认识的特殊记录类型吗？                                  */

	if (rmid == RM_DBASE_ID && rminfo == XLOG_DBASE_CREATE_FILE_COPY)
	{
		/*
		 * 新建数据库可以安全忽略。它不会出现在源系统中，
		 * 因此会被删除。但有一个边界情况：如果源系统中
		 * 也创建了一个新的、不同的数据库，我们将看到
		 * 文件已存在而不会复制它们。不过没关系；
		 * 从源系统的 WAL 重放创建新数据库的过程，
		 * 将重新复制新数据库，覆盖目标系统中
		 * 创建的数据库。                                                
		 */
	}
	else if (rmid == RM_DBASE_ID && rminfo == XLOG_DBASE_CREATE_WAL_LOG)
	{
		/*
		 * 新建数据库可以安全忽略。它不会出现在源系统中，
		 * 因此会被删除。          
		 */
	}
	else if (rmid == RM_DBASE_ID && rminfo == XLOG_DBASE_DROP)
	{
		/*
		 * 现有数据库被删除。我们将看到文件在目标数据
		 * 目录中不存在，并从源系统完整复制它们。
		 * 此处无需特殊处理。                            
		 */
	}
	else if (rmid == RM_SMGR_ID && rminfo == XLOG_SMGR_CREATE)
	{
		/*
		 * 可以安全忽略。如果源系统中不存在该文件，
		 * 它将从目标中删除。如果源系统中也创建了
		 * 同名文件，则会有 WAL 记录覆盖其中的
		 * 所有块。                                    
		 */
	}
	else if (rmid == RM_SMGR_ID && rminfo == XLOG_SMGR_TRUNCATE)
	{
		/*
		 * 可以安全忽略。稍后比较大小时会发现
		 * 它们不同，并从源系统复制
		 * 缺失的尾部。                                 
		 */
	}
	else if (rmid == RM_XACT_ID &&
			 ((rminfo & XLOG_XACT_OPMASK) == XLOG_XACT_COMMIT ||
			  (rminfo & XLOG_XACT_OPMASK) == XLOG_XACT_COMMIT_PREPARED ||
			  (rminfo & XLOG_XACT_OPMASK) == XLOG_XACT_ABORT ||
			  (rminfo & XLOG_XACT_OPMASK) == XLOG_XACT_ABORT_PREPARED))
	{
		/*
		 * 这些记录可能包含"已删除的关系"。
		 * 可以安全忽略它们，我们会发现
		 * 它们缺失并从源中复制。         
		 */
	}
	else if (info & XLR_SPECIAL_REL_UPDATE)
	{
		/*
		 * 此记录类型以某种特殊方式修改关系文件，但
		 * 我们不认识该类型。这很糟糕——我们不知道
		 * 如何跟踪该更改。  
		 */
		pg_fatal("WAL record modifies a relation, but record type is not recognized: "
				 "lsn: %X/%X, rmid: %d, rmgr: %s, info: %02X",
				 LSN_FORMAT_ARGS(record->ReadRecPtr),
				 rmid, RmgrName(rmid), info);
	}

	for (block_id = 0; block_id <= XLogRecMaxBlockId(record); block_id++)
	{
		RelFileLocator rlocator;
		ForkNumber	forknum;
		BlockNumber blkno;

		if (!XLogRecGetBlockTagExtended(record, block_id,
										&rlocator, &forknum, &blkno, NULL))
			continue;

		/* 我们只关心主分支；其他分支完整复制                                           */
		if (forknum != MAIN_FORKNUM)
			continue;

		process_target_wal_block_change(forknum, rlocator, blkno);
	}
}
