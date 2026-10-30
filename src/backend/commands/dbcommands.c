/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		数据库管理命令（创建/删除数据库）。
 *
 * 注意：数据库创建/删除命令使用数据库对象上的排他锁
 * （通过 LockSharedObject() 表达）来避免互相干扰。
 * 以前我们对 pg_database 使用表级锁，但那粒度过粗。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/dbcommands_xlog.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "replication/slot.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/pg_locale.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 创建数据库的策略。
 *
 * CREATEDB_WAL_LOG 将在块级别复制数据库，并对每个复制的块
 * 记录 WAL 日志。
 *
 * CREATEDB_FILE_COPY 将简单地在文件系统级别执行数据库复制，
 * 并为每个复制的表空间记录一条记录。为了确保安全，
 * 它还在操作前后触发检查点。
 */
typedef enum CreateDBStrategy
{
	CREATEDB_WAL_LOG,
	CREATEDB_FILE_COPY
} CreateDBStrategy;

typedef struct
{
	Oid			src_dboid;		/* 源（模板）数据库 */
	Oid			dest_dboid;		/* 我们尝试创建的数据库 */
	CreateDBStrategy strategy;	/* 创建数据库策略 */
} createdb_failure_params;

typedef struct
{
	Oid			dest_dboid;		/* 我们尝试移动的数据库 */
	Oid			dest_tsoid;		/* 我们尝试移动到的表空间 */
} movedb_failure_params;

/*
 * 创建数据库时要复制的关系的信息。
 */
typedef struct CreateDBRelInfo
{
	RelFileLocator rlocator;	/* 物理关系标识符 */
	Oid			reloid;			/* 关系的 OID */
	bool		permanent;		/* 关系是永久的还是非日志的 */
} CreateDBRelInfo;


/* 非导出函数原型 */
static void createdb_failure_callback(int code, Datum arg);
static void movedb(const char *dbname, const char *tblspcname);
static void movedb_failure_callback(int code, Datum arg);
static bool get_db_info(const char *name, LOCKMODE lockmode,
						Oid *dbIdP, Oid *ownerIdP,
						int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
						TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
						Oid *dbTablespace, char **dbCollate, char **dbCtype, char **dbIculocale,
						char **dbIcurules,
						char *dbLocProvider,
						char **dbCollversion);
static void remove_dbtablespaces(Oid db_id);
static bool check_db_file_conflict(Oid db_id);
static int	errdetail_busy_db(int notherbackends, int npreparedxacts);
static void CreateDatabaseUsingWalLog(Oid src_dboid, Oid dst_dboid, Oid src_tsid,
									  Oid dst_tsid);
static List *ScanSourceDatabasePgClass(Oid tbid, Oid dbid, char *srcpath);
static List *ScanSourceDatabasePgClassPage(Page page, Buffer buf, Oid tbid,
										   Oid dbid, char *srcpath,
										   List *rlocatorlist, Snapshot snapshot);
static CreateDBRelInfo *ScanSourceDatabasePgClassTuple(HeapTupleData *tuple,
													   Oid tbid, Oid dbid,
													   char *srcpath);
static void CreateDirAndVersionFile(char *dbpath, Oid dbid, Oid tsid,
									bool isRedo);
static void CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid,
										Oid src_tsid, Oid dst_tsid);
static void recovery_create_dbdir(char *path, bool only_tblspc);

/*
 * 使用 WAL_LOG 策略创建新数据库。
 *
 * 每个复制的块都会单独写入预写日志。
 */
static void
CreateDatabaseUsingWalLog(Oid src_dboid, Oid dst_dboid,
						  Oid src_tsid, Oid dst_tsid)
{
	char	   *srcpath;
	char	   *dstpath;
	List	   *rlocatorlist = NULL;
	ListCell   *cell;
	LockRelId	srcrelid;
	LockRelId	dstrelid;
	RelFileLocator srcrlocator;
	RelFileLocator dstrlocator;
	CreateDBRelInfo *relinfo;

	/* 获取源数据库和目标数据库路径。 */
	srcpath = GetDatabasePath(src_dboid, src_tsid);
	dstpath = GetDatabasePath(dst_dboid, dst_tsid);

	/* 创建数据库目录并写入 PG_VERSION 文件。 */
	CreateDirAndVersionFile(dstpath, dst_dboid, dst_tsid, false);

	/* 将 relmap 文件从源数据库复制到目标数据库。 */
	RelationMapCopy(dst_dboid, dst_tsid, srcpath, dstpath);

	/* 获取要从源数据库复制的 relfilelocator 列表。 */
	rlocatorlist = ScanSourceDatabasePgClass(src_tsid, src_dboid, srcpath);
	Assert(rlocatorlist != NIL);

	/*
	 * 所有关系的数据库 ID 都相同，因此进入循环前预先设置。
	 
 */
	srcrelid.dbId = src_dboid;
	dstrelid.dbId = dst_dboid;

	/* 遍历 relfilelocator 列表并复制每一个。 */
	foreach(cell, rlocatorlist)
	{
		relinfo = lfirst(cell);
		srcrlocator = relinfo->rlocator;

		/*
		 * 如果关系来自源数据库的默认表空间，则需要在目标数据库的
		 * 默认表空间中创建。否则需要在源数据库中所在的同一表空间中创建。
		 
 *
 */
		if (srcrlocator.spcOid == src_tsid)
			dstrlocator.spcOid = dst_tsid;
		else
			dstrlocator.spcOid = srcrlocator.spcOid;

		dstrlocator.dbOid = dst_dboid;
		dstrlocator.relNumber = srcrlocator.relNumber;

		/*
		 * 在复制之前获取源和目标关系的锁。
		 *
		 * 我们通常不会在未持有关系锁的情况下将关系数据读入
		 * shared_buffers。在这种情况下跳过它会发生什么问题
		 * 尚不清楚，因为此时没有人可以修改源或目标数据库，
		 * 而且我们对两个数据库都持有锁，但让我们采取保守的做法。
		 
 */
		dstrelid.relId = srcrelid.relId = relinfo->reloid;
		LockRelationId(&srcrelid, AccessShareLock);
		LockRelationId(&dstrelid, AccessShareLock);

		/* 将关系存储从源复制到目标。 */
		CreateAndCopyRelationData(srcrlocator, dstrlocator, relinfo->permanent);

		/* 释放关系锁。 */
		UnlockRelationId(&srcrelid, AccessShareLock);
		UnlockRelationId(&dstrelid, AccessShareLock);
	}

	pfree(srcpath);
	pfree(dstpath);
	list_free_deep(rlocatorlist);
}

/*
 * 扫描源数据库中的 pg_class 表，识别需要复制到
 * 目标数据库的关系。
 *
 * 这是跨数据库访问通常不可行这一常规规则的例外。
 * 我们可以在这里实现它，因为我们知道没有到源数据库的连接，
 * 并且（因为没有准备好的事务触及该数据库）也没有存疑元组。
 * 这意味着我们不需要担心清理操作会在我们脚下移除任何东西，
 * 我们也不需要过于挑剔快照。只要它将所有先前提交的 XID
 * 视为已提交，所有已中止的 XID 视为已中止，我们就没问题：
 * 在这里不可能有其他情况。
 *
 * 我们在这里不能依赖 relcache，因为它只知道我们连接到的
 * 数据库，并且无法处理对其他数据库的访问。这也意味着
 * 我们不能依赖堆扫描基础设施，这在任何情况下都是个坏主意，
 * 因为它可能会尝试进行 HOT 清理等操作，而我们绝对无法在
 * 我们甚至未连接的数据库中安全地执行这些操作。
 
 *
 *
 */
static List *
ScanSourceDatabasePgClass(Oid tbid, Oid dbid, char *srcpath)
{
	RelFileLocator rlocator;
	BlockNumber nblocks;
	BlockNumber blkno;
	Buffer		buf;
	RelFileNumber relfilenumber;
	Page		page;
	List	   *rlocatorlist = NIL;
	LockRelId	relid;
	Snapshot	snapshot;
	SMgrRelation smgr;
	BufferAccessStrategy bstrategy;

	/* 获取 pg_class 的 relfilenumber。 */
	relfilenumber = RelationMapOidToFilenumberForDatabase(srcpath,
														  RelationRelationId);

	/* 不要在未持有关系锁的情况下将数据读入 shared_buffers。 */
	relid.dbId = dbid;
	relid.relId = RelationRelationId;
	LockRelationId(&relid, AccessShareLock);

	/* 为 pg_class 关系准备 RelFileLocator。 */
	rlocator.spcOid = tbid;
	rlocator.dbOid = dbid;
	rlocator.relNumber = relfilenumber;

	smgr = smgropen(rlocator, InvalidBackendId);
	nblocks = smgrnblocks(smgr, MAIN_FORKNUM);
	smgrclose(smgr);

	/* 由于这是批量读取操作，使用缓冲区访问策略。 */
	bstrategy = GetAccessStrategy(BAS_BULKREAD);

	/*
	 * 如函数头部注释所述，我们需要一个快照将所有已提交的
	 * 事务视为已提交，而我们的事务快照（或活动快照）
	 * 可能不够新，但 GetLatestSnapshot() 的返回值应该可以。
	 
 */
	snapshot = GetLatestSnapshot();

	/* 逐块处理该关系。 */
	for (blkno = 0; blkno < nblocks; blkno++)
	{
		CHECK_FOR_INTERRUPTS();

		buf = ReadBufferWithoutRelcache(rlocator, MAIN_FORKNUM, blkno,
										RBM_NORMAL, bstrategy, true);

		LockBuffer(buf, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buf);
		if (PageIsNew(page) || PageIsEmpty(page))
		{
			UnlockReleaseBuffer(buf);
			continue;
		}

		/* 将当前页的相关 pg_class 元组追加到 rlocatorlist。 */
		rlocatorlist = ScanSourceDatabasePgClassPage(page, buf, tbid, dbid,
													 srcpath, rlocatorlist,
													 snapshot);

		UnlockReleaseBuffer(buf);
	}

	/* 释放关系锁。 */
	UnlockRelationId(&relid, AccessShareLock);

	return rlocatorlist;
}

/*
 * 扫描源数据库 pg_class 关系的一页，并将相关条目
 * 添加到 rlocatorlist。返回值是更新后的列表。
 */
static List *
ScanSourceDatabasePgClassPage(Page page, Buffer buf, Oid tbid, Oid dbid,
							  char *srcpath, List *rlocatorlist,
							  Snapshot snapshot)
{
	BlockNumber blkno = BufferGetBlockNumber(buf);
	OffsetNumber offnum;
	OffsetNumber maxoff;
	HeapTupleData tuple;

	maxoff = PageGetMaxOffsetNumber(page);

	/* 遍历偏移量。 */
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemid;

		itemid = PageGetItemId(page, offnum);

		/* 如果槽为空或已死亡，则无需处理。 */
		if (!ItemIdIsUsed(itemid) || ItemIdIsDead(itemid) ||
			ItemIdIsRedirected(itemid))
			continue;

		Assert(ItemIdIsNormal(itemid));
		ItemPointerSet(&(tuple.t_self), blkno, offnum);

		/* 初始化 HeapTupleData 结构。 */
		tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
		tuple.t_len = ItemIdGetLength(itemid);
		tuple.t_tableOid = RelationRelationId;

		/* 跳过对此快照不可见的元组。 */
		if (HeapTupleSatisfiesVisibility(&tuple, snapshot, buf))
		{
			CreateDBRelInfo *relinfo;

			/*
			 * ScanSourceDatabasePgClassTuple 负责为此元组构造
			 * CreateDBRelInfo 对象，但也可以判断不需要复制
			 * 此元组。如果确实需要复制该关系，则将其添加到列表中。
			 
 */
			relinfo = ScanSourceDatabasePgClassTuple(&tuple, tbid, dbid,
													 srcpath);
			if (relinfo != NULL)
				rlocatorlist = lappend(rlocatorlist, relinfo);
		}
	}

	return rlocatorlist;
}

/*
 * 判断某个 pg_class 元组是否表示需要从源数据库复制到
 * 目标数据库的内容，如果是，则为其构造
 * CreateDBRelInfo。
 *
 * 可见性检查由调用方处理，因此我们这里的工作只是
 * 评估元组中存储的数据。
 */
CreateDBRelInfo *
ScanSourceDatabasePgClassTuple(HeapTupleData *tuple, Oid tbid, Oid dbid,
							   char *srcpath)
{
	CreateDBRelInfo *relinfo;
	Form_pg_class classForm;
	RelFileNumber relfilenumber = InvalidRelFileNumber;

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/*
	 * 返回 NULL 如果此对象不需要复制。
	 *
	 * 共享对象不需要复制，因为它们是共享的。
	 * 没有存储的对象无法复制，因为没有内容可复制。
	 * 临时关系也不需要复制，因为它们在创建它们的
	 * 会话之外不可访问，该会话必定已经消失，
	 * 即使仍存在也无法连接到其他数据库。autovacuum
	 * 最终也会移除这些 pg_class 条目。
	 
 */
	if (classForm->reltablespace == GLOBALTABLESPACE_OID ||
		!RELKIND_HAS_STORAGE(classForm->relkind) ||
		classForm->relpersistence == RELPERSISTENCE_TEMP)
		return NULL;

	/*
	 * 如果 relfilenumber 有效则直接使用。否则查询 relmap。
	 
 */
	if (RelFileNumberIsValid(classForm->relfilenode))
		relfilenumber = classForm->relfilenode;
	else
		relfilenumber = RelationMapOidToFilenumberForDatabase(srcpath,
															  classForm->oid);

	/* 我们必须拥有有效的 relfilenumber。 */
	if (!RelFileNumberIsValid(relfilenumber))
		elog(ERROR, "relation with OID %u does not have a valid relfilenumber",
			 classForm->oid);

	/* 准备关系信息元素并将其添加到列表中。 */
	relinfo = (CreateDBRelInfo *) palloc(sizeof(CreateDBRelInfo));
	if (OidIsValid(classForm->reltablespace))
		relinfo->rlocator.spcOid = classForm->reltablespace;
	else
		relinfo->rlocator.spcOid = tbid;

	relinfo->rlocator.dbOid = dbid;
	relinfo->rlocator.relNumber = relfilenumber;
	relinfo->reloid = classForm->oid;

	/* 临时关系已在上面被拒绝。 */
	Assert(classForm->relpersistence != RELPERSISTENCE_TEMP);
	relinfo->permanent =
		(classForm->relpersistence == RELPERSISTENCE_PERMANENT) ? true : false;

	return relinfo;
}

/*
 * 创建数据库目录并在数据库路径中写出 PG_VERSION 文件。
 * 如果 isRedo 为 true，数据库目录已存在也没问题。
 
 */
static void
CreateDirAndVersionFile(char *dbpath, Oid dbid, Oid tsid, bool isRedo)
{
	int			fd;
	int			nbytes;
	char		versionfile[MAXPGPATH];
	char		buf[16];

	/*
	 * 注意我们不必从源数据库复制版本数据；
	 * 只有一个合法的值。
	 */
	sprintf(buf, "%s\n", PG_MAJORVERSION);
	nbytes = strlen(PG_MAJORVERSION) + 1;

	/* 创建数据库目录。 */
	if (MakePGDirectory(dbpath) < 0)
	{
		/* 除了已存在或不在 WAL 重放中之外的其他失败？ */
		if (errno != EEXIST || !isRedo)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m", dbpath)));
	}

	/*
	 * 在数据库路径中创建 PG_VERSION 文件。如果文件已
	 * 存在且我们处于 WAL 重放中，则再次尝试以写模式打开。
	 
 */
	snprintf(versionfile, sizeof(versionfile), "%s/%s", dbpath, "PG_VERSION");

	fd = OpenTransientFile(versionfile, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (fd < 0 && errno == EEXIST && isRedo)
		fd = OpenTransientFile(versionfile, O_WRONLY | O_TRUNC | PG_BINARY);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", versionfile)));

	/* 在 PG_VERSION 文件中写入 PG_MAJORVERSION。 */
	pgstat_report_wait_start(WAIT_EVENT_VERSION_FILE_WRITE);
	errno = 0;
	if ((int) write(fd, buf, nbytes) != nbytes)
	{
		/* 如果 write 没有设置 errno，则假设问题是磁盘空间不足。 */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", versionfile)));
	}
	pgstat_report_wait_end();

	pgstat_report_wait_start(WAIT_EVENT_VERSION_FILE_SYNC);
	if (pg_fsync(fd) != 0)
		ereport(data_sync_elevel(ERROR),
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", versionfile)));
	fsync_fname(dbpath, true);
	pgstat_report_wait_end();

	/* 关闭版本文件。 */
	CloseTransientFile(fd);

	/* 如果我们不在 WAL 重放中，则写入 WAL 记录。 */
	if (!isRedo)
	{
		xl_dbase_create_wal_log_rec xlrec;

		START_CRIT_SECTION();

		xlrec.db_id = dbid;
		xlrec.tablespace_id = tsid;

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec),
						 sizeof(xl_dbase_create_wal_log_rec));

		(void) XLogInsert(RM_DBASE_ID, XLOG_DBASE_CREATE_WAL_LOG);

		END_CRIT_SECTION();
	}
}

/*
 * 使用 FILE_COPY 策略创建新数据库。
 *
 * 在文件系统级别复制每个表空间，并为每个复制的表空间
 * 记录一条 WAL 记录。这需要在复制前后各执行一次
 * 检查点，开销可能很大，但如果复制的数据库很大，
 * 可以大大减少 WAL 生成量。
 */
static void
CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid, Oid src_tsid,
							Oid dst_tsid)
{
	TableScanDesc scan;
	Relation	rel;
	HeapTuple	tuple;

	/*
	 * 在开始复制前强制执行检查点。这将强制将所有脏缓冲区
	 * （包括无日志表的）刷新到磁盘，以确保源数据库在磁盘上
	 * 是最新的。FlushDatabaseBuffers() 能做到这一点，但
	 * 我们还需要处理任何待处理的 unlink 请求。否则，
	 * 如果在复制文件时发生检查点，某个文件可能在我们
	 * 即将复制它时被删除，导致 copydir() 中的
	 * lstat() 调用因 ENOENT 失败。
	 
 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE |
					  CHECKPOINT_WAIT | CHECKPOINT_FLUSH_ALL);

	/*
	 * 遍历模板数据库的所有表空间，并将每个表空间复制到
	 * 新数据库。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spaceform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			srctablespace = spaceform->oid;
		Oid			dsttablespace;
		char	   *srcpath;
		char	   *dstpath;
		struct stat st;

		/* 无需复制全局表空间 */
		if (srctablespace == GLOBALTABLESPACE_OID)
			continue;

		srcpath = GetDatabasePath(src_dboid, srctablespace);

		if (stat(srcpath, &st) < 0 || !S_ISDIR(st.st_mode) ||
			directory_is_empty(srcpath))
		{
			/* 假设可以忽略 */
			pfree(srcpath);
			continue;
		}

		if (srctablespace == src_tsid)
			dsttablespace = dst_tsid;
		else
			dsttablespace = srctablespace;

		dstpath = GetDatabasePath(dst_dboid, dsttablespace);

		/*
		 * 将此子目录复制到新位置
		 *
		 * 我们不需要复制子目录
		 */
		copydir(srcpath, dstpath, false);

		/* 在 XLOG 中记录文件系统变更 */
		{
			xl_dbase_create_file_copy_rec xlrec;

			xlrec.db_id = dst_dboid;
			xlrec.tablespace_id = dsttablespace;
			xlrec.src_db_id = src_dboid;
			xlrec.src_tablespace_id = srctablespace;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec,
							 sizeof(xl_dbase_create_file_copy_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_CREATE_FILE_COPY | XLR_SPECIAL_REL_UPDATE);
		}
		pfree(srcpath);
		pfree(dstpath);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	/*
	 * 在提交前强制执行检查点。这实际上意味着已提交的
	 * XLOG_DBASE_CREATE_FILE_COPY 操作永远不需要重放
	 * （至少在普通崩溃恢复中不需要；我们仍然需要为
	 * PITR 操作创建 XLOG 条目）。这避免了两种糟糕的情况：
	 *
	 * #1: 当 PITR 关闭时，我们不会 XLOG 新创建索引的内容；
	 * 因此 DBASE_CREATE 重放的
	 * "删除并重建整个目录"行为会丢失这些索引。
	 *
	 * #2: 由于我们必须在 DBASE_CREATE 重放期间重新复制
	 * 源数据库，我们冒着复制在原始 CREATE DATABASE 命令
	 * 之后但在导致重放的系统崩溃之前提交的变更的风险。
	 * 这至少是意外行为，最坏情况下可能导致不一致，
	 * 例如重复的表名。
	 *
	 * （这两个都是 8.0 到 8.0.3 版本中的真实 bug。）
	 *
	 * 在 PITR 重放中，第一个不是问题，第二个只有在
	 * CREATE DATABASE 和后续模板数据库变更都在进行
	 * 基础备份时发生才有风险。似乎除了将其记录为
	 * 限制外，我们对此无能为力。
	 *
	 * 参见 CreateDatabaseUsingWalLog() 获取不那么
	 * 粗糙的 CREATE DATABASE 策略，该策略避免了这些问题。
	 
 *
 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
}

/*
 * CREATE DATABASE
 */
Oid
createdb(ParseState *pstate, const CreatedbStmt *stmt)
{
	Oid			src_dboid;
	Oid			src_owner;
	int			src_encoding = -1;
	char	   *src_collate = NULL;
	char	   *src_ctype = NULL;
	char	   *src_iculocale = NULL;
	char	   *src_icurules = NULL;
	char		src_locprovider = '\0';
	char	   *src_collversion = NULL;
	bool		src_istemplate;
	bool		src_allowconn;
	TransactionId src_frozenxid = InvalidTransactionId;
	MultiXactId src_minmxid = InvalidMultiXactId;
	Oid			src_deftablespace;
	volatile Oid dst_deftablespace;
	Relation	pg_database_rel;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_database] = {0};
	bool		new_record_nulls[Natts_pg_database] = {0};
	Oid			dboid = InvalidOid;
	Oid			datdba;
	ListCell   *option;
	DefElem    *dtablespacename = NULL;
	DefElem    *downer = NULL;
	DefElem    *dtemplate = NULL;
	DefElem    *dencoding = NULL;
	DefElem    *dlocale = NULL;
	DefElem    *dcollate = NULL;
	DefElem    *dctype = NULL;
	DefElem    *diculocale = NULL;
	DefElem    *dicurules = NULL;
	DefElem    *dlocprovider = NULL;
	DefElem    *distemplate = NULL;
	DefElem    *dallowconnections = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *dcollversion = NULL;
	DefElem    *dstrategy = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	const char *dbtemplate = NULL;
	char	   *dbcollate = NULL;
	char	   *dbctype = NULL;
	char	   *dbiculocale = NULL;
	char	   *dbicurules = NULL;
	char		dblocprovider = '\0';
	char	   *canonname;
	int			encoding = -1;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = DATCONNLIMIT_UNLIMITED;
	char	   *dbcollversion = NULL;
	int			notherbackends;
	int			npreparedxacts;
	CreateDBStrategy dbstrategy = CREATEDB_WAL_LOG;
	createdb_failure_params fparms;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (dtablespacename)
				errorConflictingDefElem(defel, pstate);
			dtablespacename = defel;
		}
		else if (strcmp(defel->defname, "owner") == 0)
		{
			if (downer)
				errorConflictingDefElem(defel, pstate);
			downer = defel;
		}
		else if (strcmp(defel->defname, "template") == 0)
		{
			if (dtemplate)
				errorConflictingDefElem(defel, pstate);
			dtemplate = defel;
		}
		else if (strcmp(defel->defname, "encoding") == 0)
		{
			if (dencoding)
				errorConflictingDefElem(defel, pstate);
			dencoding = defel;
		}
		else if (strcmp(defel->defname, "locale") == 0)
		{
			if (dlocale)
				errorConflictingDefElem(defel, pstate);
			dlocale = defel;
		}
		else if (strcmp(defel->defname, "lc_collate") == 0)
		{
			if (dcollate)
				errorConflictingDefElem(defel, pstate);
			dcollate = defel;
		}
		else if (strcmp(defel->defname, "lc_ctype") == 0)
		{
			if (dctype)
				errorConflictingDefElem(defel, pstate);
			dctype = defel;
		}
		else if (strcmp(defel->defname, "icu_locale") == 0)
		{
			if (diculocale)
				errorConflictingDefElem(defel, pstate);
			diculocale = defel;
		}
		else if (strcmp(defel->defname, "icu_rules") == 0)
		{
			if (dicurules)
				errorConflictingDefElem(defel, pstate);
			dicurules = defel;
		}
		else if (strcmp(defel->defname, "locale_provider") == 0)
		{
			if (dlocprovider)
				errorConflictingDefElem(defel, pstate);
			dlocprovider = defel;
		}
		else if (strcmp(defel->defname, "is_template") == 0)
		{
			if (distemplate)
				errorConflictingDefElem(defel, pstate);
			distemplate = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (dallowconnections)
				errorConflictingDefElem(defel, pstate);
			dallowconnections = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "collation_version") == 0)
		{
			if (dcollversion)
				errorConflictingDefElem(defel, pstate);
			dcollversion = defel;
		}
		else if (strcmp(defel->defname, "location") == 0)
		{
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("LOCATION is not supported anymore"),
					 errhint("Consider using tablespaces instead."),
					 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "oid") == 0)
		{
			dboid = defGetObjectId(defel);

			/*
			 * 我们通常不允许使用系统分配的 OID 创建新数据库。
			 * pg_upgrade 尝试保留数据库 OID，因此我们不能
			 * 允许使用某个未来版本创建的新初始化集簇中可能
			 * 已在使用的 OID 创建数据库。我们假设所有此类
			 * OID 来自系统管理的 OID 范围。
			 *
			 * 但作为例外，当
			 * allow_system_table_mods=on 时（使 initdb 可以
			 * 为 template0 和 postgres 分配系统 OID）或执行
			 * 二进制升级时（使 pg_upgrade 可以保留源集簇中
			 * 找到的任何 OID），我们允许分配任何 OID。
			 
 */
			if (dboid < FirstNormalObjectId &&
				!allowSystemTableMods && !IsBinaryUpgrade)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
						errmsg("OIDs less than %u are reserved for system objects", FirstNormalObjectId));
		}
		else if (strcmp(defel->defname, "strategy") == 0)
		{
			if (dstrategy)
				errorConflictingDefElem(defel, pstate);
			dstrategy = defel;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	if (downer && downer->arg)
		dbowner = defGetString(downer);
	if (dtemplate && dtemplate->arg)
		dbtemplate = defGetString(dtemplate);
	if (dencoding && dencoding->arg)
	{
		const char *encoding_name;

		if (IsA(dencoding->arg, Integer))
		{
			encoding = defGetInt32(dencoding);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 ||
				pg_valid_server_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code",
								encoding),
						 parser_errposition(pstate, dencoding->location)));
		}
		else
		{
			encoding_name = defGetString(dencoding);
			encoding = pg_valid_server_encoding(encoding_name);
			if (encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name",
								encoding_name),
						 parser_errposition(pstate, dencoding->location)));
		}
	}
	if (dlocale && dlocale->arg)
	{
		dbcollate = defGetString(dlocale);
		dbctype = defGetString(dlocale);
	}
	if (dcollate && dcollate->arg)
		dbcollate = defGetString(dcollate);
	if (dctype && dctype->arg)
		dbctype = defGetString(dctype);
	if (diculocale && diculocale->arg)
		dbiculocale = defGetString(diculocale);
	if (dicurules && dicurules->arg)
		dbicurules = defGetString(dicurules);
	if (dlocprovider && dlocprovider->arg)
	{
		char	   *locproviderstr = defGetString(dlocprovider);

		if (pg_strcasecmp(locproviderstr, "icu") == 0)
			dblocprovider = COLLPROVIDER_ICU;
		else if (pg_strcasecmp(locproviderstr, "libc") == 0)
			dblocprovider = COLLPROVIDER_LIBC;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("unrecognized locale provider: %s",
							locproviderstr)));
	}
	if (distemplate && distemplate->arg)
		dbistemplate = defGetBoolean(distemplate);
	if (dallowconnections && dallowconnections->arg)
		dballowconnections = defGetBoolean(dallowconnections);
	if (dconnlimit && dconnlimit->arg)
	{
		dbconnlimit = defGetInt32(dconnlimit);
		if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}
	if (dcollversion)
		dbcollversion = defGetString(dcollversion);

	/* obtain OID of proposed owner */
	if (dbowner)
		datdba = get_role_oid(dbowner, false);
	else
		datdba = GetUserId();

	/*
	 * 要创建数据库，必须具有 createdb 权限并且必须能够
	 * 成为目标角色（这并不意味着目标角色本身必须具有
	 * createdb 权限）。后一项规定防范
	 * "赠予"攻击。注意超级用户总是同时具有这两项特权。
	 
 */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create database")));

	check_can_set_role(GetUserId(), datdba);

	/*
	 * 查找要克隆的数据库（模板），并获取其共享锁。
	 * ShareLock 允许两个 CREATE DATABASE 并发地从同一
	 * 模板工作，同时确保没有人在并行地删除它（这将
	 * 非常糟糕，因为我们可能在不知情的情况下获得不完整
	 * 的副本）。这也阻止了在完成复制之前对源数据库
	 * 建立任何新连接，因此我们可以确保它不会在我们
	 * 脚下发生变化。
	 */
	if (!dbtemplate)
		dbtemplate = "template1";	/* Default template database name */

	if (!get_db_info(dbtemplate, ShareLock,
					 &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_allowconn,
					 &src_frozenxid, &src_minmxid, &src_deftablespace,
					 &src_collate, &src_ctype, &src_iculocale, &src_icurules, &src_locprovider,
					 &src_collversion))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("template database \"%s\" does not exist",
						dbtemplate)));

	/*
	 * 如果源数据库正在被删除，则不能将其用作模板。
	 
 */
	if (database_is_invalid_oid(src_dboid))
		ereport(ERROR,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot use invalid database \"%s\" as template", dbtemplate),
				errhint("Use DROP DATABASE to drop invalid databases."));

	/*
	 * 权限检查：要复制未标记为 datistemplate 的数据库，
	 * 您必须是超级用户或其所有者。
	 */
	if (!src_istemplate)
	{
		if (!object_ownercheck(DatabaseRelationId, src_dboid, GetUserId()))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to copy database \"%s\"",
							dbtemplate)));
	}

	/* Validate the database creation strategy. */
	if (dstrategy && dstrategy->arg)
	{
		char	   *strategy;

		strategy = defGetString(dstrategy);
		if (pg_strcasecmp(strategy, "wal_log") == 0)
			dbstrategy = CREATEDB_WAL_LOG;
		else if (pg_strcasecmp(strategy, "file_copy") == 0)
			dbstrategy = CREATEDB_FILE_COPY;
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid create database strategy \"%s\"", strategy),
					 errhint("Valid strategies are \"wal_log\" and \"file_copy\".")));
	}

	/* If encoding or locales are defaulted, use source's setting */
	if (encoding < 0)
		encoding = src_encoding;
	if (dbcollate == NULL)
		dbcollate = src_collate;
	if (dbctype == NULL)
		dbctype = src_ctype;
	if (dblocprovider == '\0')
		dblocprovider = src_locprovider;
	if (dbiculocale == NULL && dblocprovider == COLLPROVIDER_ICU)
	{
		if (dlocale && dlocale->arg)
			dbiculocale = defGetString(dlocale);
		else
			dbiculocale = src_iculocale;
	}
	if (dbicurules == NULL && dblocprovider == COLLPROVIDER_ICU)
		dbicurules = src_icurules;

	/* Some encodings are client only */
	if (!PG_VALID_BE_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid server encoding %d", encoding)));

	/* Check that the chosen locales are valid, and get canonical spellings */
	if (!check_locale(LC_COLLATE, dbcollate, &canonname))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate),
				 errhint("If the locale name is specific to ICU, use ICU_LOCALE.")));
	dbcollate = canonname;
	if (!check_locale(LC_CTYPE, dbctype, &canonname))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype),
				 errhint("If the locale name is specific to ICU, use ICU_LOCALE.")));
	dbctype = canonname;

	check_encoding_locale_matches(encoding, dbcollate, dbctype);

	if (dblocprovider == COLLPROVIDER_ICU)
	{
		if (!(is_encoding_supported_by_icu(encoding)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("encoding \"%s\" is not supported with ICU provider",
							pg_encoding_to_char(encoding))));

		/*
		 * 如果 template0 使用 libc 提供程序但新数据库
		 * 使用 icu，则会发生这种情况。
		 */
		if (!dbiculocale)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("LOCALE or ICU_LOCALE must be specified")));

		/*
		 * 在二进制升级期间，或当语言环境来自模板数据库时，
		 * 保留语言环境字符串。否则，规范化为语言标签。
		 
 */
		if (!IsBinaryUpgrade && dbiculocale != src_iculocale)
		{
			char	   *langtag = icu_language_tag(dbiculocale,
												   icu_validation_level);

			if (langtag && strcmp(dbiculocale, langtag) != 0)
			{
				ereport(NOTICE,
						(errmsg("using standard form \"%s\" for ICU locale \"%s\"",
								langtag, dbiculocale)));

				dbiculocale = langtag;
			}
		}

		icu_validate_locale(dbiculocale);
	}
	else
	{
		if (dbiculocale)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU locale cannot be specified unless locale provider is ICU")));

		if (dbicurules)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("ICU rules cannot be specified unless locale provider is ICU")));
	}

	/*
	 * 检查新编码和语言环境设置是否与源数据库匹配。
	 * 我们坚持这样做，因为我们只是复制源数据 ---
	 * 任何非 ASCII 数据会被错误编码，任何按源语言环境
	 * 排序的索引会出错。
	 *
	 * 但是，我们假设 template0 不包含任何非 ASCII 数据
	 * 或依赖排序规则/ctype 的索引，因此 template0 可以
	 * 用作创建具有任何编码或语言环境数据库的模板。
	 */
	if (strcmp(dbtemplate, "template0") != 0)
	{
		if (encoding != src_encoding)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new encoding (%s) is incompatible with the encoding of the template database (%s)",
							pg_encoding_to_char(encoding),
							pg_encoding_to_char(src_encoding)),
					 errhint("Use the same encoding as in the template database, or use template0 as template.")));

		if (strcmp(dbcollate, src_collate) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new collation (%s) is incompatible with the collation of the template database (%s)",
							dbcollate, src_collate),
					 errhint("Use the same collation as in the template database, or use template0 as template.")));

		if (strcmp(dbctype, src_ctype) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new LC_CTYPE (%s) is incompatible with the LC_CTYPE of the template database (%s)",
							dbctype, src_ctype),
					 errhint("Use the same LC_CTYPE as in the template database, or use template0 as template.")));

		if (dblocprovider != src_locprovider)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new locale provider (%s) does not match locale provider of the template database (%s)",
							collprovider_name(dblocprovider), collprovider_name(src_locprovider)),
					 errhint("Use the same locale provider as in the template database, or use template0 as template.")));

		if (dblocprovider == COLLPROVIDER_ICU)
		{
			char	   *val1;
			char	   *val2;

			Assert(dbiculocale);
			Assert(src_iculocale);
			if (strcmp(dbiculocale, src_iculocale) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("new ICU locale (%s) is incompatible with the ICU locale of the template database (%s)",
								dbiculocale, src_iculocale),
						 errhint("Use the same ICU locale as in the template database, or use template0 as template.")));

			val1 = dbicurules;
			if (!val1)
				val1 = "";
			val2 = src_icurules;
			if (!val2)
				val2 = "";
			if (strcmp(val1, val2) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("new ICU collation rules (%s) are incompatible with the ICU collation rules of the template database (%s)",
								val1, val2),
						 errhint("Use the same ICU collation rules as in the template database, or use template0 as template.")));
		}
	}

	/*
	 * 如果我们获取了模板数据库的排序规则版本，检查它是否
	 * 与实际操作系统排序规则版本匹配。否则报错；用户需要
	 * 先修复模板数据库。如果排序规则版本作为语句选项
	 * 显式指定，则不要抱怨；这由 pg_upgrade 用于精确
	 * 重现旧状态。
	 *
	 * （如果模板数据库没有排序规则版本，则要么
	 * 平台/提供程序不支持排序规则版本控制，要么它是
	 * template0，我们规定它不包含使用排序规则的对象。）
	 
 */
	if (src_collversion && !dcollversion)
	{
		char	   *actual_versionstr;

		actual_versionstr = get_collation_actual_version(dblocprovider, dblocprovider == COLLPROVIDER_ICU ? dbiculocale : dbcollate);
		if (!actual_versionstr)
			ereport(ERROR,
					(errmsg("template database \"%s\" has a collation version, but no actual collation version could be determined",
							dbtemplate)));

		if (strcmp(actual_versionstr, src_collversion) != 0)
			ereport(ERROR,
					(errmsg("template database \"%s\" has a collation version mismatch",
							dbtemplate),
					 errdetail("The template database was created using collation version %s, "
							   "but the operating system provides version %s.",
							   src_collversion, actual_versionstr),
					 errhint("Rebuild all objects in the template database that use the default collation and run "
							 "ALTER DATABASE %s REFRESH COLLATION VERSION, "
							 "or build PostgreSQL with the right library version.",
							 quote_identifier(dbtemplate))));
	}

	if (dbcollversion == NULL)
		dbcollversion = src_collversion;

	/*
	 * 通常，我们从模板数据库复制排序规则版本。
	 * 此最后手段仅在模板数据库没有排序规则版本时适用，
	 * 这通常只对 template0 成立。
	 */
	if (dbcollversion == NULL)
		dbcollversion = get_collation_actual_version(dblocprovider, dblocprovider == COLLPROVIDER_ICU ? dbiculocale : dbcollate);

	/* Resolve default tablespace for new database */
	if (dtablespacename && dtablespacename->arg)
	{
		char	   *tablespacename;
		AclResult	aclresult;

		tablespacename = defGetString(dtablespacename);
		dst_deftablespace = get_tablespace_oid(tablespacename, false);
		/* check permissions */
		aclresult = object_aclcheck(TableSpaceRelationId, dst_deftablespace, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_TABLESPACE,
						   tablespacename);

		/* pg_global must never be the default tablespace */
		if (dst_deftablespace == GLOBALTABLESPACE_OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_global cannot be used as default tablespace")));

		/*
		 * 如果我们尝试更改模板的默认表空间，则要求模板
		 * 在新的默认表空间中没有任何文件。这是必要的，
		 * 因为否则复制的数据库将包含同时显式（通过 OID）
		 * 和隐式（作为零）引用其默认表空间的 pg_class
		 * 行，这会导致问题。例如，另一个使用复制的数据库
		 * 作为模板并再次尝试更改其默认表空间的
		 * CREATE DATABASE 会产生完全错误的结果（它会
		 * 不正确地将在同一表空间中保留的表移动到新的
		 * 默认表空间）。
		 
 */
		if (dst_deftablespace != src_deftablespace)
		{
			char	   *srcpath;
			struct stat st;

			srcpath = GetDatabasePath(src_dboid, dst_deftablespace);

			if (stat(srcpath, &st) == 0 &&
				S_ISDIR(st.st_mode) &&
				!directory_is_empty(srcpath))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot assign new default tablespace \"%s\"",
								tablespacename),
						 errdetail("There is a conflict because database \"%s\" already has some tables in this tablespace.",
								   dbtemplate)));
			pfree(srcpath);
		}
	}
	else
	{
		/* Use template database's default tablespace */
		dst_deftablespace = src_deftablespace;
		/* Note there is no additional permission check in this path */
	}

	/*
	 * 如果使用适当的开关构建，当违反回归测试的数据库名称
	 * 约定时发出警告。但在 initdb 期间不要抱怨。
	 
 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (IsUnderPostmaster && strstr(dbname, "regression") == NULL)
		elog(WARNING, "databases created by regression test cases should have names including \"regression\"");
#endif

	/*
	 * 检查数据库名称冲突。这只是为了提供比
	 * "唯一索引冲突"更友好的错误消息。存在竞争条件，
	 * 但在此情况下我们愿意接受不太友好的消息。
	 */
	if (OidIsValid(get_database_oid(dbname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));

	/*
	 * 源数据库不能有任何活动后端，除了当前这个
	 * （例外是允许在连接到 template1 时 CREATE DB）。
	 * 否则我们可能会复制不一致的数据。
	 *
	 * 这应该是基本错误检查中的最后一项，因为它涉及
	 * 潜在等待；如果要抛出错误，还不如先抛出。
	 
 */
	if (CountOtherDBBackends(src_dboid, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("source database \"%s\" is being accessed by other users",
						dbtemplate),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * 为新数据库选择一个 OID，检查它是否与表空间目录中
	 * 已有的任何内容有文件名冲突。
	 
 */
	pg_database_rel = table_open(DatabaseRelationId, RowExclusiveLock);

	/*
	 * 如果配置了数据库 OID，检查该 OID 是否已被使用或
	 * 数据目录是否已存在。
	 */
	if (OidIsValid(dboid))
	{
		char	   *existing_dbname = get_database_name(dboid);

		if (existing_dbname != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
					errmsg("database OID %u is already in use by database \"%s\"",
						   dboid, existing_dbname));

		if (check_db_file_conflict(dboid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
					errmsg("data directory with the specified OID %u already exists", dboid));
	}
	else
	{
		/* Select an OID for the new database if is not explicitly configured. */
		do
		{
			dboid = GetNewOidWithIndex(pg_database_rel, DatabaseOidIndexId,
									   Anum_pg_database_oid);
		} while (check_db_file_conflict(dboid));
	}

	/*
	 * 向 pg_database 插入新元组。这建立了我们对新数据库
	 * 名称的所有权（任何其他尝试插入相同名称的人将在
	 * 唯一索引上阻塞，并在我们提交后失败）。
	 */

	Assert((dblocprovider == COLLPROVIDER_ICU && dbiculocale) ||
		   (dblocprovider != COLLPROVIDER_ICU && !dbiculocale));

	/* Form tuple */
	new_record[Anum_pg_database_oid - 1] = ObjectIdGetDatum(dboid);
	new_record[Anum_pg_database_datname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbname));
	new_record[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(datdba);
	new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
	new_record[Anum_pg_database_datlocprovider - 1] = CharGetDatum(dblocprovider);
	new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
	new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
	new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
	new_record[Anum_pg_database_datfrozenxid - 1] = TransactionIdGetDatum(src_frozenxid);
	new_record[Anum_pg_database_datminmxid - 1] = TransactionIdGetDatum(src_minmxid);
	new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_deftablespace);
	new_record[Anum_pg_database_datcollate - 1] = CStringGetTextDatum(dbcollate);
	new_record[Anum_pg_database_datctype - 1] = CStringGetTextDatum(dbctype);
	if (dbiculocale)
		new_record[Anum_pg_database_daticulocale - 1] = CStringGetTextDatum(dbiculocale);
	else
		new_record_nulls[Anum_pg_database_daticulocale - 1] = true;
	if (dbicurules)
		new_record[Anum_pg_database_daticurules - 1] = CStringGetTextDatum(dbicurules);
	else
		new_record_nulls[Anum_pg_database_daticurules - 1] = true;
	if (dbcollversion)
		new_record[Anum_pg_database_datcollversion - 1] = CStringGetTextDatum(dbcollversion);
	else
		new_record_nulls[Anum_pg_database_datcollversion - 1] = true;

	/*
	 * 我们特意将 datacl 设置为默认值（NULL），而不是从
	 * 模板数据库复制它。当所有者与模板的所有者不同时，
	 * 复制它会是个坏主意。
	 */
	new_record_nulls[Anum_pg_database_datacl - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(pg_database_rel),
							new_record, new_record_nulls);

	CatalogTupleInsert(pg_database_rel, tuple);

	/*
	 * 现在生成与新数据库关联的额外目录条目
	 */

	/* Register owner dependency */
	recordDependencyOnOwner(DatabaseRelationId, dboid, datdba);

	/* Create pg_shdepend entries for objects within database */
	copyTemplateDependencies(src_dboid, dboid);

	/* Post creation hook for new database */
	InvokeObjectPostCreateHook(DatabaseRelationId, dboid, 0);

	/*
	 * 如果我们要将要创建的数据库的数据读入 shared_buffers，
	 * 则获取其锁。此时应该没有人知道这个数据库存在，
	 * 但保持以下不变式是好的：数据库上的
	 * AccessExclusiveLock 足以删除其所有缓冲区，而无需
	 * 担心稍后有更多数据被读入。
	 *
	 * 注意我们需要在进入下面的 PG_ENSURE_ERROR_CLEANUP
	 * 块之前执行此操作，因为 createdb_failure_callback
	 * 期望此锁已被持有。
	 */
	if (dbstrategy == CREATEDB_WAL_LOG)
		LockSharedObject(DatabaseRelationId, dboid, 0, AccessShareLock);

	/*
	 * 一旦开始复制子目录，如果失败，我们需要能够清理
	 * 它们。使用 ENSURE 块确保发生清理。（这不是 100%
	 * 的解决方案，因为在我们离开此例程后事务提交期间
	 * 也可能失败，但它应该处理大多数场景。）
	 
 */
	fparms.src_dboid = src_dboid;
	fparms.dest_dboid = dboid;
	fparms.strategy = dbstrategy;

	PG_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
							PointerGetDatum(&fparms));
	{
		/*
		 * 如果用户要求使用 WAL_LOG 策略创建数据库，则调用
		 * CreateDatabaseUsingWalLog，它将在块级别复制
		 * 数据库，并为每个复制的块记录 WAL。
		 * 否则，调用 CreateDatabaseUsingFileCopy，它将
		 * 逐文件复制数据库。
		 */
		if (dbstrategy == CREATEDB_WAL_LOG)
			CreateDatabaseUsingWalLog(src_dboid, dboid, src_deftablespace,
									  dst_deftablespace);
		else
			CreateDatabaseUsingFileCopy(src_dboid, dboid, src_deftablespace,
										dst_deftablespace);

		/*
		 * 关闭 pg_database，但保持锁定直到提交。
		 */
		table_close(pg_database_rel, NoLock);

		/*
		 * 强制同步提交，从而最小化数据库文件创建和事务
		 * 提交之间的窗口。如果我们在提交前崩溃，将有一个
		 * 占用磁盘空间但不在 pg_database 中的数据库，
		 * 这不好。
		 */
		ForceSyncCommit();
	}
	PG_END_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
								PointerGetDatum(&fparms));

	return dboid;
}

/*
 * 检查选择的编码是否与选择的语言环境设置匹配。此限制
 * 是必要的，因为 libc 的语言环境特定代码在收到不期望的
 * 编码数据时通常会失败。我们在四种情况下允许不匹配：
 *
 * 1. 语言环境编码 = SQL_ASCII，意味着语言环境是 C/POSIX，
 * 可以与任何编码一起工作。
 *
 * 2. 语言环境编码 = -1，意味着我们无法确定语言环境的
 * 编码，必须信任用户能正确设置。
 *
 * 3. 选择的编码是 UTF8 且平台是 win32。这是因为 UTF8
 * 是一个伪代码页，在所有语言环境中都受支持，因为它在
 * 使用前会转换为 UTF16。
 *
 * 4. 选择的编码是 SQL_ASCII，但仅限超级用户。这有风险，
 * 但我们历史上一直允许 --- 值得注意的是，回归测试
 * 需要它。
 *
 * 注意：如果更改此策略，请相应修改 initdb。
 
 */
void
check_encoding_locale_matches(int encoding, const char *collate, const char *ctype)
{
	int			ctype_encoding = pg_get_encoding_from_locale(ctype, true);
	int			collate_encoding = pg_get_encoding_from_locale(collate, true);

	if (!(ctype_encoding == encoding ||
		  ctype_encoding == PG_SQL_ASCII ||
		  ctype_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						ctype),
				 errdetail("The chosen LC_CTYPE setting requires encoding \"%s\".",
						   pg_encoding_to_char(ctype_encoding))));

	if (!(collate_encoding == encoding ||
		  collate_encoding == PG_SQL_ASCII ||
		  collate_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						collate),
				 errdetail("The chosen LC_COLLATE setting requires encoding \"%s\".",
						   pg_encoding_to_char(collate_encoding))));
}

/* Error cleanup callback for createdb */
static void
createdb_failure_callback(int code, Datum arg)
{
	createdb_failure_params *fparms = (createdb_failure_params *) DatumGetPointer(arg);

	/*
	 * 如果我们在块级别复制数据库，则删除共享缓冲区
	 * 缓存中目标数据库的页面。并告知 checkpointer
	 * 忘记数据库中文件的任何待处理 fsync 和 unlink 请求。
	 * 这样做背后的原因与 dropdb 函数中解释的相同。
	 * 但与 dropdb 不同，我们不需要调用
	 * pgstat_drop_database，因为此数据库尚未创建，
	 * 因此不应该有任何统计信息。
	 */
	if (fparms->strategy == CREATEDB_WAL_LOG)
	{
		DropDatabaseBuffers(fparms->dest_dboid);
		ForgetDatabaseSyncRequests(fparms->dest_dboid);

		/* Release lock on the target database. */
		UnlockSharedObject(DatabaseRelationId, fparms->dest_dboid, 0,
						   AccessShareLock);
	}

	/*
	 * 在执行递归删除之前释放源数据库的锁。这不是必需的，
	 * 但似乎应该尽快释放锁。
	 
 */
	UnlockSharedObject(DatabaseRelationId, fparms->src_dboid, 0, ShareLock);

	/* Throw away any successfully copied subdirectories */
	remove_dbtablespaces(fparms->dest_dboid);
}


/*
 * DROP DATABASE
 */
void
dropdb(const char *dbname, bool missing_ok, bool force)
{
	Oid			db_id;
	bool		db_istemplate;
	Relation	pgdbrel;
	HeapTuple	tup;
	ScanKeyData scankey;
	void	   *inplace_state;
	Form_pg_database datform;
	int			notherbackends;
	int			npreparedxacts;
	int			nslots,
				nslots_active;
	int			nsubscriptions;

	/*
	 * 查找目标数据库的 OID，并获取其排他锁。我们需要
	 * 此锁来确保在删除目标数据库时没有新后端启动
	 * （参见 postinit.c），并且没有人将其用作
	 * CREATE DATABASE 模板或试图自行删除它。
	 
 */
	pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
					 &db_istemplate, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		}
		else
		{
			/* Close pg_database, release the lock, since we changed nothing */
			table_close(pgdbrel, RowExclusiveLock);
			ereport(NOTICE,
					(errmsg("database \"%s\" does not exist, skipping",
							dbname)));
			return;
		}
	}

	/*
	 * 权限检查
	 */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   dbname);

	/* DROP hook for the database being removed */
	InvokeObjectDropHook(DatabaseRelationId, db_id, 0);

	/*
	 * 禁止删除标记为 istemplate 的数据库。这只是为了防止
	 * 人们意外删除 template0 或 template1；如果他们真的
	 * 执意要删除，也可以 ...
	 */
	if (db_istemplate)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop a template database")));

	/* Obviously can't drop my own database */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the currently open database")));

	/*
	 * 检查是否存在引用待删除数据库的活动逻辑插槽。
	 * 我们持有的数据库锁阻止使用该数据库创建新插槽
	 * 或现有插槽变为活动状态。
	 
 */
	(void) ReplicationSlotsCountDBSlots(db_id, &nslots, &nslots_active);
	if (nslots_active)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is used by an active logical replication slot",
						dbname),
				 errdetail_plural("There is %d active slot.",
								  "There are %d active slots.",
								  nslots_active, nslots_active)));
	}

	/*
	 * 检查目标数据库中是否定义了订阅。
	 *
	 * 我们不能自动删除它们，因为它们可能持有其他
	 * 数据库/实例中的资源。
	 */
	if ((nsubscriptions = CountDBSubscriptions(db_id)) > 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being used by logical replication subscription",
						dbname),
				 errdetail_plural("There is %d subscription.",
								  "There are %d subscriptions.",
								  nsubscriptions, nsubscriptions)));


	/*
	 * 如果用户请求，尝试终止到目标数据库的所有现有连接。
	 
 */
	if (force)
		TerminateOtherDBBackends(db_id);

	/*
	 * 检查目标数据库中的其他后端。（因为我们持有数据库
	 * 锁，在此之后没有新的可以启动。）
	 *
	 * 与 CREATE DATABASE 一样，在其他错误条件之后检查此项。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * 删除与数据库关联的任何注释或安全标签。
	 */
	DeleteSharedComments(db_id, DatabaseRelationId);
	DeleteSharedSecurityLabel(db_id, DatabaseRelationId);

	/*
	 * 删除与此数据库关联的设置
	 */
	DropSetting(db_id, InvalidOid);

	/*
	 * 删除数据库的共享依赖引用。
	 */
	dropDatabaseDependencies(db_id);

	/*
	 * 告知累积统计系统也立即忘记它。
	 */
	pgstat_drop_database(db_id);

	/*
	 * 除了删除目录行外，后续操作不是事务性的（考虑
	 * DropDatabaseBuffers() 丢弃已修改的缓冲区）。但
	 * 我们可能在下面崩溃或中断。为防止访问内容无效的
	 * 数据库，使用就地更新将数据库标记为无效。
	 *
	 * 我们需要在继续之前刷新 WAL，以保证修改在执行
	 * 不可逆文件系统操作之前是持久的。
	 
 *
 */
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	systable_inplace_update_begin(pgdbrel, DatabaseNameIndexId, true,
								  NULL, 1, &scankey, &tup, &inplace_state);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", db_id);
	datform = (Form_pg_database) GETSTRUCT(tup);
	datform->datconnlimit = DATCONNLIMIT_INVALID_DB;
	systable_inplace_update_finish(inplace_state, tup);
	XLogFlush(XactLastRecEnd);

	/*
	 * 同时事务性地删除元组。如果此事务提交，行将消失，
	 * 但如果失败，可以再次调用 dropdb()。
	 */
	CatalogTupleDelete(pgdbrel, &tup->t_self);
	heap_freetuple(tup);

	/*
	 * 删除数据库特定的复制槽。
	 */
	ReplicationSlotsDropDBSlots(db_id);

	/*
	 * 删除共享缓冲区缓存中此数据库的页面。这很重要，
	 * 以确保没有剩余后端稍后尝试将脏缓冲区写出到
	 * 已死数据库 ...
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * 告知 checkpointer 忘记数据库中文件的任何待处理
	 * fsync 和 unlink 请求；否则 fsync 将在下次检查点
	 * 失败，或更糟的是，将删除属于具有相同 OID 的
	 * 新创建数据库的文件。
	 */
	ForgetDatabaseSyncRequests(db_id);

	/*
	 * 强制执行检查点以确保 checkpointer 已收到
	 * ForgetDatabaseSyncRequests 发送的消息。
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

	/* Close all smgr fds in all backends. */
	WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

	/*
	 * 删除属于数据库的所有表空间子目录。
	 */
	remove_dbtablespaces(db_id);

	/*
	 * 关闭 pg_database，但保持锁定直到提交。
	 */
	table_close(pgdbrel, NoLock);

	/*
	 * 强制同步提交，从而最小化数据库文件删除和事务
	 * 提交之间的窗口。如果我们在提交前崩溃，将有一个
	 * 在磁盘上已消失但根据 pg_database 仍存在的数据库，
	 * 这不好。
	 */
	ForceSyncCommit();
}


/*
 * 重命名数据库
 */
ObjectAddress
RenameDatabase(const char *oldname, const char *newname)
{
	Oid			db_id;
	HeapTuple	newtup;
	ItemPointerData otid;
	Relation	rel;
	int			notherbackends;
	int			npreparedxacts;
	ObjectAddress address;

	/*
	 * 查找目标数据库的 OID，并获取其排他锁。我们
	 * 出于与 DROP DATABASE 相同的原因需要此锁。
	 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(oldname, AccessExclusiveLock, &db_id, NULL, NULL,
					 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", oldname)));

	/* must be owner */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   oldname);

	/* must have createdb rights */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename database")));

	/*
	 * 如果使用适当的开关构建，当违反回归测试的数据库
	 * 名称约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strstr(newname, "regression") == NULL)
		elog(WARNING, "databases created by regression test cases should have names including \"regression\"");
#endif

	/*
	 * 确保新名称不存在。参见 CREATE DATABASE 中相同
	 * 错误的说明。
	 */
	if (OidIsValid(get_database_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", newname)));

	/*
	 * XXX 客户端应用程序可能在某处存储了当前数据库名称，
	 * 因此重命名它可能会导致混淆。另一方面，除了
	 * 一点混淆之外，可能没有实际问题，所以请思考并决定。
	 
 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current database cannot be renamed")));

	/*
	 * 确保数据库没有活动会话。这与上面的担忧相同，
	 * 但适用于其他会话。
	 *
	 * 与 CREATE DATABASE 一样，在其他错误条件之后检查此项。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						oldname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/* rename */
	newtup = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(db_id));
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "cache lookup failed for database %u", db_id);
	otid = newtup->t_self;
	namestrcpy(&(((Form_pg_database) GETSTRUCT(newtup))->datname), newname);
	CatalogTupleUpdate(rel, &otid, newtup);
	UnlockTuple(rel, &otid, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	/*
	 * 关闭 pg_database，但保持锁定直到提交。
	 */
	table_close(rel, NoLock);

	return address;
}


/*
 * ALTER DATABASE SET TABLESPACE
 */
static void
movedb(const char *dbname, const char *tblspcname)
{
	Oid			db_id;
	Relation	pgdbrel;
	int			notherbackends;
	int			npreparedxacts;
	HeapTuple	oldtuple,
				newtuple;
	Oid			src_tblspcoid,
				dst_tblspcoid;
	ScanKeyData scankey;
	SysScanDesc sysscan;
	AclResult	aclresult;
	char	   *src_dbpath;
	char	   *dst_dbpath;
	DIR		   *dstdir;
	struct dirent *xlde;
	movedb_failure_params fparms;

	/*
	 * 查找目标数据库的 OID，并获取其排他锁。我们需要
	 * 此锁来确保在移动数据库时没有新后端启动，并且
	 * 没有人将其用作 CREATE DATABASE 模板或试图删除它。
	 
 */
	pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
					 NULL, NULL, NULL, NULL, &src_tblspcoid, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	/*
	 * 我们实际上需要一个会话锁，以便锁在下面的
	 * commit/restart 之后持续存在。（我们几乎可以
	 * 让锁在提交时释放，除了有人可能会在我们
	 * rmtree() 旧目录时尝试将数据库的关系移回旧目录。）
	 */
	LockSharedObjectForSession(DatabaseRelationId, db_id, 0,
							   AccessExclusiveLock);

	/*
	 * 权限检查
	 */
	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   dbname);

	/*
	 * 显然不能移动自己数据库的表空间
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot change the tablespace of the currently open database")));

	/*
	 * 获取表空间的 oid
	 */
	dst_tblspcoid = get_tablespace_oid(tblspcname, false);

	/*
	 * 权限检查
	 */
	aclresult = object_aclcheck(TableSpaceRelationId, dst_tblspcoid, GetUserId(),
								ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_TABLESPACE,
					   tblspcname);

	/*
	 * pg_global 绝不能是默认表空间
	 */
	if (dst_tblspcoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_global cannot be used as default tablespace")));

	/*
	 * 如果是相同表空间则无操作
	 */
	if (src_tblspcoid == dst_tblspcoid)
	{
		table_close(pgdbrel, NoLock);
		UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
									 AccessExclusiveLock);
		return;
	}

	/*
	 * 检查目标数据库中的其他后端。（因为我们持有数据库
	 * 锁，在此之后没有新的可以启动。）
	 *
	 * 与 CREATE DATABASE 一样，在其他错误条件之后检查此项。
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * 获取旧的和新的数据库路径
	 */
	src_dbpath = GetDatabasePath(db_id, src_tblspcoid);
	dst_dbpath = GetDatabasePath(db_id, dst_tblspcoid);

	/*
	 * 在继续之前强制执行检查点。这将强制将所有脏缓冲区
	 * （包括无日志表的）刷新到磁盘，以确保源数据库
	 * 在磁盘上是最新的。FlushDatabaseBuffers() 能够
	 * 做到这一点，但我们还需要处理任何待处理的 unlink
	 * 请求。否则，目标目录中现有文件的检查可能不必要
	 * 地失败，更不用说复制可能因源文件在复制过程中
	 * 被删除而失败。在 Windows 上，这也确保后台进程
	 * 不持有任何打开文件，否则会导致 rmdir() 失败。
	 
 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT
					  | CHECKPOINT_FLUSH_ALL);

	/* Close all smgr fds in all backends. */
	WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

	/*
	 * 现在丢弃所有包含目标数据库数据的缓冲区；它们应该
	 * 不再是脏的了，因此 DropDatabaseBuffers 是安全的。
	 *
	 * 看起来我们可以让这些缓冲区自然地从共享缓冲区中
	 * 老化，因为它们不应该再被引用。问题在于，如果用户
	 * 稍后将数据库移回其原始表空间，任何仍然存活的
	 * 缓冲区似乎又会包含有效数据 --- 但它们会丢失
	 * 数据库在新表空间中发生的任何更改。无论如何，
	 * 释放永远不应再使用的缓冲区似乎是值得的。
	 *
	 * 注意：只要去除与 db_id 和 src_tblspcoid 匹配的
	 * 缓冲区就足够了，但 bufmgr.c 目前没有提供
	 * 相应的 API。
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * 检查目标目录中是否存在文件，即已存在于目标
	 * 表空间中的此数据库的对象。我们无法在这种情况下
	 * 允许移动，因为我们需要将这些关系的
	 * pg_class.reltablespace 条目更改为零，而我们
	 * 无法访问数据库的 pg_class 来执行此操作。
	 */
	dstdir = AllocateDir(dst_dbpath);
	if (dstdir != NULL)
	{
		while ((xlde = ReadDir(dstdir, dst_dbpath)) != NULL)
		{
			if (strcmp(xlde->d_name, ".") == 0 ||
				strcmp(xlde->d_name, "..") == 0)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("some relations of database \"%s\" are already in tablespace \"%s\"",
							dbname, tblspcname),
					 errhint("You must move them back to the database's default tablespace before using this command.")));
		}

		FreeDir(dstdir);

		/*
		 * 目录存在但为空。我们必须在调用 copydir 函数
		 * 之前删除它。
		 */
		if (rmdir(dst_dbpath) != 0)
			elog(ERROR, "could not remove directory \"%s\": %m",
				 dst_dbpath);
	}

	/*
	 * 使用 ENSURE 块确保在复制失败时（例如磁盘空间不足）
	 * 清除碎片。这不是 100% 的解决方案，因为事务提交
	 * 期间也可能失败，但它应该处理大多数场景。
	 
 */
	fparms.dest_dboid = db_id;
	fparms.dest_tsoid = dst_tblspcoid;
	PG_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
							PointerGetDatum(&fparms));
	{
		Datum		new_record[Natts_pg_database] = {0};
		bool		new_record_nulls[Natts_pg_database] = {0};
		bool		new_record_repl[Natts_pg_database] = {0};

		/*
		 * 将文件从旧表空间复制到新表空间
		 */
		copydir(src_dbpath, dst_dbpath, false);

		/*
		 * 在 XLOG 中记录文件系统变更
		 */
		{
			xl_dbase_create_file_copy_rec xlrec;

			xlrec.db_id = db_id;
			xlrec.tablespace_id = dst_tblspcoid;
			xlrec.src_db_id = db_id;
			xlrec.src_tablespace_id = src_tblspcoid;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec,
							 sizeof(xl_dbase_create_file_copy_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_CREATE_FILE_COPY | XLR_SPECIAL_REL_UPDATE);
		}

		/*
		 * 更新数据库的 pg_database 元组
		 */
		ScanKeyInit(&scankey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(dbname));
		sysscan = systable_beginscan(pgdbrel, DatabaseNameIndexId, true,
									 NULL, 1, &scankey);
		oldtuple = systable_getnext(sysscan);
		if (!HeapTupleIsValid(oldtuple))	/* shouldn't happen... */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		LockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

		new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_tblspcoid);
		new_record_repl[Anum_pg_database_dattablespace - 1] = true;

		newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(pgdbrel),
									 new_record,
									 new_record_nulls, new_record_repl);
		CatalogTupleUpdate(pgdbrel, &oldtuple->t_self, newtuple);
		UnlockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

		InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

		systable_endscan(sysscan);

		/*
		 * 在此处强制执行另一个检查点。与 CREATE DATABASE
		 * 一样，这是为了确保我们不必重放已提交的
		 * XLOG_DBASE_CREATE_FILE_COPY 操作，这会导致我们
		 * 丢失在下一个检查点之前在新数据库表空间中执行的
		 * 任何无日志操作。
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * 强制同步提交，从而最小化数据库文件复制和事务
		 * 提交之间的窗口。如果我们在提交前崩溃，将在磁盘上
		 * 留下孤立的一组文件，这不是致命的但也不好。
		 
 */
		ForceSyncCommit();

		/*
		 * 关闭 pg_database，但保持锁定直到提交。
		 */
		table_close(pgdbrel, NoLock);
	}
	PG_END_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
								PointerGetDatum(&fparms));

	/*
	 * 提交事务以使 pg_database 更新被提交。如果我们在
	 * 删除文件时崩溃，数据库不会损坏，我们只会在旧目录
	 * 中留下一些孤立文件。
	 *
	 * （这没问题，因为我们知道不在事务块中。）
	 *
	 * XXX 在 ensure 块内执行此操作是否安全/更好？不认为
	 * 这是个好主意；考虑在事务真正提交后立即 elog。
	 
 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Start new transaction for the remaining work; don't need a snapshot */
	StartTransactionCommand();

	/*
	 * 从旧表空间中删除文件
	 */
	if (!rmtree(src_dbpath, true))
		ereport(WARNING,
				(errmsg("some useless files may be left behind in old database directory \"%s\"",
						src_dbpath)));

	/*
	 * 在 XLOG 中记录文件系统变更
	 */
	{
		xl_dbase_drop_rec xlrec;

		xlrec.db_id = db_id;
		xlrec.ntablespaces = 1;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_dbase_drop_rec));
		XLogRegisterData((char *) &src_tblspcoid, sizeof(Oid));

		(void) XLogInsert(RM_DBASE_ID,
						  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
	}

	/* Now it's safe to release the database lock */
	UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
								 AccessExclusiveLock);

	pfree(src_dbpath);
	pfree(dst_dbpath);
}

/* Error cleanup callback for movedb */
static void
movedb_failure_callback(int code, Datum arg)
{
	movedb_failure_params *fparms = (movedb_failure_params *) DatumGetPointer(arg);
	char	   *dstpath;

	/* Get rid of anything we managed to copy to the target directory */
	dstpath = GetDatabasePath(fparms->dest_dboid, fparms->dest_tsoid);

	(void) rmtree(dstpath, true);

	pfree(dstpath);
}

/*
 * 处理选项并调用 dropdb 函数。
 */
void
DropDatabase(ParseState *pstate, DropdbStmt *stmt)
{
	bool		force = false;
	ListCell   *lc;

	foreach(lc, stmt->options)
	{
		DefElem    *opt = (DefElem *) lfirst(lc);

		if (strcmp(opt->defname, "force") == 0)
			force = true;
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized DROP DATABASE option \"%s\"", opt->defname),
					 parser_errposition(pstate, opt->location)));
	}

	dropdb(stmt->dbname, stmt->missing_ok, force);
}

/*
 * ALTER DATABASE name ...
 */
Oid
AlterDatabase(ParseState *pstate, AlterDatabaseStmt *stmt, bool isTopLevel)
{
	Relation	rel;
	Oid			dboid;
	HeapTuple	tuple,
				newtuple;
	Form_pg_database datform;
	ScanKeyData scankey;
	SysScanDesc scan;
	ListCell   *option;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = DATCONNLIMIT_UNLIMITED;
	DefElem    *distemplate = NULL;
	DefElem    *dallowconnections = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *dtablespace = NULL;
	Datum		new_record[Natts_pg_database] = {0};
	bool		new_record_nulls[Natts_pg_database] = {0};
	bool		new_record_repl[Natts_pg_database] = {0};

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "is_template") == 0)
		{
			if (distemplate)
				errorConflictingDefElem(defel, pstate);
			distemplate = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (dallowconnections)
				errorConflictingDefElem(defel, pstate);
			dallowconnections = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (dconnlimit)
				errorConflictingDefElem(defel, pstate);
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (dtablespace)
				errorConflictingDefElem(defel, pstate);
			dtablespace = defel;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	if (dtablespace)
	{
		/*
		 * 虽然 SET TABLESPACE 语法不允许任何其他选项，
		 * 但有人可能会写 "WITH TABLESPACE ..."。在这种情况下，
		 * 禁止指定任何其他选项。
		 */
		if (list_length(stmt->options) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("option \"%s\" cannot be specified with other options",
							dtablespace->defname),
					 parser_errposition(pstate, dtablespace->location)));
		/* this case isn't allowed within a transaction block */
		PreventInTransactionBlock(isTopLevel, "ALTER DATABASE SET TABLESPACE");
		movedb(stmt->dbname, defGetString(dtablespace));
		return InvalidOid;
	}

	if (distemplate && distemplate->arg)
		dbistemplate = defGetBoolean(distemplate);
	if (dallowconnections && dallowconnections->arg)
		dballowconnections = defGetBoolean(dallowconnections);
	if (dconnlimit && dconnlimit->arg)
	{
		dbconnlimit = defGetInt32(dconnlimit);
		if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}

	/*
	 * 获取旧元组。我们不需要数据库本身的锁，因为我们
	 * 不会做任何会扰乱传入连接的事情。
	 
 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));
	LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	datform = (Form_pg_database) GETSTRUCT(tuple);
	dboid = datform->oid;

	if (database_is_invalid_form(datform))
	{
		ereport(FATAL,
				errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				errmsg("cannot alter invalid database \"%s\"", stmt->dbname),
				errhint("Use DROP DATABASE to drop invalid databases."));
	}

	if (!object_ownercheck(DatabaseRelationId, dboid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);

	/*
	 * 为了避免被锁定在外并不得不通过单用户模式，我们
	 * 拒绝对当前连接的数据库禁止连接。锁定仍然可能
	 * 在并发会话中发生，但可能性不高，不值得担心。
	 
 */
	if (!dballowconnections && dboid == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot disallow connections for current database")));

	/*
	 * 构建更新后的元组，使用刚刚获取的信息
	 */
	if (distemplate)
	{
		new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
		new_record_repl[Anum_pg_database_datistemplate - 1] = true;
	}
	if (dallowconnections)
	{
		new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
		new_record_repl[Anum_pg_database_datallowconn - 1] = true;
	}
	if (dconnlimit)
	{
		new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
		new_record_repl[Anum_pg_database_datconnlimit - 1] = true;
	}

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), new_record,
								 new_record_nulls, new_record_repl);
	CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
	UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, dboid, 0);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	table_close(rel, NoLock);

	return dboid;
}


/*
 * ALTER DATABASE name REFRESH COLLATION VERSION
 */
ObjectAddress
AlterDatabaseRefreshColl(AlterDatabaseRefreshCollStmt *stmt)
{
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Oid			db_id;
	HeapTuple	tuple;
	Form_pg_database datForm;
	ObjectAddress address;
	Datum		datum;
	bool		isnull;
	char	   *oldversion;
	char	   *newversion;

	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	datForm = (Form_pg_database) GETSTRUCT(tuple);
	db_id = datForm->oid;

	if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);
	LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	datum = heap_getattr(tuple, Anum_pg_database_datcollversion, RelationGetDescr(rel), &isnull);
	oldversion = isnull ? NULL : TextDatumGetCString(datum);

	datum = heap_getattr(tuple, datForm->datlocprovider == COLLPROVIDER_ICU ? Anum_pg_database_daticulocale : Anum_pg_database_datcollate, RelationGetDescr(rel), &isnull);
	if (isnull)
		elog(ERROR, "unexpected null in pg_database");
	newversion = get_collation_actual_version(datForm->datlocprovider, TextDatumGetCString(datum));

	/* cannot change from NULL to non-NULL or vice versa */
	if ((!oldversion && newversion) || (oldversion && !newversion))
		elog(ERROR, "invalid collation version change");
	else if (oldversion && newversion && strcmp(newversion, oldversion) != 0)
	{
		bool		nulls[Natts_pg_database] = {0};
		bool		replaces[Natts_pg_database] = {0};
		Datum		values[Natts_pg_database] = {0};
		HeapTuple	newtuple;

		ereport(NOTICE,
				(errmsg("changing version from %s to %s",
						oldversion, newversion)));

		values[Anum_pg_database_datcollversion - 1] = CStringGetTextDatum(newversion);
		replaces[Anum_pg_database_datcollversion - 1] = true;

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
									 values, nulls, replaces);
		CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
		heap_freetuple(newtuple);
	}
	else
		ereport(NOTICE,
				(errmsg("version has not changed")));
	UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	systable_endscan(scan);

	table_close(rel, NoLock);

	return address;
}


/*
 * ALTER DATABASE name SET ...
 */
Oid
AlterDatabaseSet(AlterDatabaseSetStmt *stmt)
{
	Oid			datid = get_database_oid(stmt->dbname, false);

	/*
	 * 获取数据库的锁，并确保它在此期间没有消失。
	 
 */
	shdepLockAndCheckObject(DatabaseRelationId, datid);

	if (!object_ownercheck(DatabaseRelationId, datid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
					   stmt->dbname);

	AlterSetting(datid, InvalidOid, stmt->setstmt);

	UnlockSharedObject(DatabaseRelationId, datid, 0, AccessShareLock);

	return datid;
}


/*
 * ALTER DATABASE name OWNER TO newowner
 */
ObjectAddress
AlterDatabaseOwner(const char *dbname, Oid newOwnerId)
{
	Oid			db_id;
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Form_pg_database datForm;
	ObjectAddress address;

	/*
	 * 获取旧元组。我们不需要数据库本身的锁，因为我们
	 * 不会做任何会扰乱传入连接的事情。
	 
 */
	rel = table_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	datForm = (Form_pg_database) GETSTRUCT(tuple);
	db_id = datForm->oid;

	/*
	 * 如果新所有者与现有所有者相同，则认为命令成功。
	 * 这是为了与其他对象保持一致。
	 
 */
	if (datForm->datdba != newOwnerId)
	{
		Datum		repl_val[Natts_pg_database];
		bool		repl_null[Natts_pg_database] = {0};
		bool		repl_repl[Natts_pg_database] = {0};
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be owner of the existing object */
		if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE,
						   dbname);

		/* Must be able to become new owner */
		check_can_set_role(GetUserId(), newOwnerId);

		/*
		 * 必须具有 createdb 权限
		 *
		 * 注意：这不同于其他 alter-owner 检查，
		 * 因为检查的是当前用户的 createdb 权限，而不是
		 * 目标所有者。这与数据库的 CREATE 情况一致。
		 * 由于超级用户总是具有此权限，我们不需要
		 * 为他们设置特殊情况。
		 */
		if (!have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to change owner of database")));

		LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

		repl_repl[Anum_pg_database_datdba - 1] = true;
		repl_val[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * 确定新所有者的修改后的 ACL。这仅在 ACL
		 * 非空时才需要。
		 */
		aclDatum = heap_getattr(tuple,
								Anum_pg_database_datacl,
								RelationGetDescr(rel),
								&isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 datForm->datdba, newOwnerId);
			repl_repl[Anum_pg_database_datacl - 1] = true;
			repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);
		UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(DatabaseRelationId, db_id, newOwnerId);
	}

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	table_close(rel, NoLock);

	return address;
}


Datum
pg_database_collation_actual_version(PG_FUNCTION_ARGS)
{
	Oid			dbid = PG_GETARG_OID(0);
	HeapTuple	tp;
	char		datlocprovider;
	Datum		datum;
	char	   *version;

	tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database with OID %u does not exist", dbid)));

	datlocprovider = ((Form_pg_database) GETSTRUCT(tp))->datlocprovider;

	datum = SysCacheGetAttrNotNull(DATABASEOID, tp, datlocprovider == COLLPROVIDER_ICU ? Anum_pg_database_daticulocale : Anum_pg_database_datcollate);
	version = get_collation_actual_version(datlocprovider, TextDatumGetCString(datum));

	ReleaseSysCache(tp);

	if (version)
		PG_RETURN_TEXT_P(cstring_to_text(version));
	else
		PG_RETURN_NULL();
}


/*
 * 辅助函数
 */

/*
 * 查找名为 "name" 的数据库的信息。如果数据库存在，
 * 获取其指定锁类型，填充任何非 NULL 的剩余参数，
 * 并返回 true。如果没有这样的数据库，返回 false。
 
 */
static bool
get_db_info(const char *name, LOCKMODE lockmode,
			Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
			TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
			Oid *dbTablespace, char **dbCollate, char **dbCtype, char **dbIculocale,
			char **dbIcurules,
			char *dbLocProvider,
			char **dbCollversion)
{
	bool		result = false;
	Relation	relation;

	Assert(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	relation = table_open(DatabaseRelationId, AccessShareLock);

	/*
	 * 循环处理数据库在我们锁定它之前被重命名的罕见情况。
	 * 我们再次尝试，以防能找到同名的新的数据库。
	 
 */
	for (;;)
	{
		ScanKeyData scanKey;
		SysScanDesc scan;
		HeapTuple	tuple;
		Oid			dbOid;

		/*
		 * 没有按名称索引的数据库 syscache，因此必须用笨办法
		 
 */
		ScanKeyInit(&scanKey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(name));

		scan = systable_beginscan(relation, DatabaseNameIndexId, true,
								  NULL, 1, &scanKey);

		tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
		{
			/* definitely no database of that name */
			systable_endscan(scan);
			break;
		}

		dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		systable_endscan(scan);

		/*
		 * 现在我们有了数据库 OID，可以尝试锁定数据库。
		 */
		if (lockmode != NoLock)
			LockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);

		/*
		 * 现在，通过 OID 重新获取元组。如果它仍然存在且
		 * 仍然是相同名称，则我们胜出；否则，放弃锁并
		 * 循环回去重试。
		 */
		tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

			if (strcmp(name, NameStr(dbform->datname)) == 0)
			{
				Datum		datum;
				bool		isnull;

				/* oid of the database */
				if (dbIdP)
					*dbIdP = dbOid;
				/* oid of the owner */
				if (ownerIdP)
					*ownerIdP = dbform->datdba;
				/* character encoding */
				if (encodingP)
					*encodingP = dbform->encoding;
				/* allowed as template? */
				if (dbIsTemplateP)
					*dbIsTemplateP = dbform->datistemplate;
				/* allowing connections? */
				if (dbAllowConnP)
					*dbAllowConnP = dbform->datallowconn;
				/* limit of frozen XIDs */
				if (dbFrozenXidP)
					*dbFrozenXidP = dbform->datfrozenxid;
				/* minimum MultiXactId */
				if (dbMinMultiP)
					*dbMinMultiP = dbform->datminmxid;
				/* default tablespace for this database */
				if (dbTablespace)
					*dbTablespace = dbform->dattablespace;
				/* default locale settings for this database */
				if (dbLocProvider)
					*dbLocProvider = dbform->datlocprovider;
				if (dbCollate)
				{
					datum = SysCacheGetAttrNotNull(DATABASEOID, tuple, Anum_pg_database_datcollate);
					*dbCollate = TextDatumGetCString(datum);
				}
				if (dbCtype)
				{
					datum = SysCacheGetAttrNotNull(DATABASEOID, tuple, Anum_pg_database_datctype);
					*dbCtype = TextDatumGetCString(datum);
				}
				if (dbIculocale)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_daticulocale, &isnull);
					if (isnull)
						*dbIculocale = NULL;
					else
						*dbIculocale = TextDatumGetCString(datum);
				}
				if (dbIcurules)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_daticurules, &isnull);
					if (isnull)
						*dbIcurules = NULL;
					else
						*dbIcurules = TextDatumGetCString(datum);
				}
				if (dbCollversion)
				{
					datum = SysCacheGetAttr(DATABASEOID, tuple, Anum_pg_database_datcollversion, &isnull);
					if (isnull)
						*dbCollversion = NULL;
					else
						*dbCollversion = TextDatumGetCString(datum);
				}
				ReleaseSysCache(tuple);
				result = true;
				break;
			}
			/* can only get here if it was just renamed */
			ReleaseSysCache(tuple);
		}

		if (lockmode != NoLock)
			UnlockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);
	}

	table_close(relation, AccessShareLock);

	return result;
}

/* Check if current user has createdb privileges */
bool
have_createdb_privilege(void)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers can always do everything */
	if (superuser())
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetUserId()));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreatedb;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * 删除表空间目录
 *
 * 我们不知道 db_id 正在使用哪些表空间，因此遍历所有
 * 表空间，删除 <tablespace>/db_id
 */
static void
remove_dbtablespaces(Oid db_id)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *ltblspc = NIL;
	ListCell   *cell;
	int			ntblspc;
	int			i;
	Oid		   *tablespace_ids;

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			dsttablespace = spcform->oid;
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) < 0 || !S_ISDIR(st.st_mode))
		{
			/* Assume we can ignore it */
			pfree(dstpath);
			continue;
		}

		if (!rmtree(dstpath, true))
			ereport(WARNING,
					(errmsg("some useless files may be left behind in old database directory \"%s\"",
							dstpath)));

		ltblspc = lappend_oid(ltblspc, dsttablespace);
		pfree(dstpath);
	}

	ntblspc = list_length(ltblspc);
	if (ntblspc == 0)
	{
		table_endscan(scan);
		table_close(rel, AccessShareLock);
		return;
	}

	tablespace_ids = (Oid *) palloc(ntblspc * sizeof(Oid));
	i = 0;
	foreach(cell, ltblspc)
		tablespace_ids[i++] = lfirst_oid(cell);

	/* Record the filesystem change in XLOG */
	{
		xl_dbase_drop_rec xlrec;

		xlrec.db_id = db_id;
		xlrec.ntablespaces = ntblspc;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, MinSizeOfDbaseDropRec);
		XLogRegisterData((char *) tablespace_ids, ntblspc * sizeof(Oid));

		(void) XLogInsert(RM_DBASE_ID,
						  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
	}

	list_free(ltblspc);
	pfree(tablespace_ids);

	table_endscan(scan);
	table_close(rel, AccessShareLock);
}

/*
 * 检查是否存在与提议的新数据库 OID 冲突的现有文件；
 * 如果有，返回 true
 *
 * 如果在任何表空间中存在与提议的新 OID 匹配的子目录，
 * 我们将因重复名称而导致创建失败 ... 然后我们将在
 * remove_dbtablespaces 的清理过程中尝试删除该已存在的
 * 子目录。清除现有文件似乎是个坏主意，因此我们改为
 * 在确定新数据库的 OID 之前进行此额外检查。这与
 * GetNewRelFileNumber() 对表 relfilenumber 值所做的
 * 完全一致。
 */
static bool
check_db_file_conflict(Oid db_id)
{
	bool		result = false;
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;

	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
		Oid			dsttablespace = spcform->oid;
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) == 0)
		{
			/* Found a conflicting file (or directory, whatever) */
			pfree(dstpath);
			result = true;
			break;
		}

		pfree(dstpath);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * 为繁忙的数据库发出合适的 errdetail 消息
 */
static int
errdetail_busy_db(int notherbackends, int npreparedxacts)
{
	if (notherbackends > 0 && npreparedxacts > 0)

		/*
		 * 我们在这里不处理单复数，因为 gettext 不支持
		 * 一个字符串中的多个复数形式。
		 */
		errdetail("There are %d other session(s) and %d prepared transaction(s) using the database.",
				  notherbackends, npreparedxacts);
	else if (notherbackends > 0)
		errdetail_plural("There is %d other session using the database.",
						 "There are %d other sessions using the database.",
						 notherbackends,
						 notherbackends);
	else
		errdetail_plural("There is %d prepared transaction using the database.",
						 "There are %d prepared transactions using the database.",
						 npreparedxacts,
						 npreparedxacts);
	return 0;					/* just to keep ereport macro happy */
}

/*
 * get_database_oid - 给定数据库名称，查找 OID
 *
 * 如果 missing_ok 为 false，数据库名称未找到时抛出错误。
 * 如果为 true，则仅返回 InvalidOid。
 */
Oid
get_database_oid(const char *dbname, bool missing_ok)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	/*
	 * pg_database 没有按名称索引的 syscache，因此我们必须
	 * 用笨办法查找。
	 */
	pg_database = table_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndexId, true,
							  NULL, 1, entry);

	dbtuple = systable_getnext(scan);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(dbtuple))
		oid = ((Form_pg_database) GETSTRUCT(dbtuple))->oid;
	else
		oid = InvalidOid;

	systable_endscan(scan);
	table_close(pg_database, AccessShareLock);

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist",
						dbname)));

	return oid;
}


/*
 * get_database_name - 给定数据库 OID，查找名称
 *
 * 返回 palloc'd 字符串，如果没有这样的数据库则返回 NULL。
 */
char *
get_database_name(Oid dbid)
{
	HeapTuple	dbtuple;
	char	   *result;

	dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (HeapTupleIsValid(dbtuple))
	{
		result = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(dbtuple))->datname));
		ReleaseSysCache(dbtuple);
	}
	else
		result = NULL;

	return result;
}


/*
 * 在删除数据库时，pg_database 行被标记为无效，但
 * 目录内容仍然存在。不允许连接到这样的数据库。
 
 */
bool
database_is_invalid_form(Form_pg_database datform)
{
	return datform->datconnlimit == DATCONNLIMIT_INVALID_DB;
}


/*
 * database_is_invalid_form() 的便利包装
 */
bool
database_is_invalid_oid(Oid dboid)
{
	HeapTuple	dbtup;
	Form_pg_database dbform;
	bool		invalid;

	dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
	if (!HeapTupleIsValid(dbtup))
		elog(ERROR, "cache lookup failed for database %u", dboid);
	dbform = (Form_pg_database) GETSTRUCT(dbtup);

	invalid = database_is_invalid_form(dbform);

	ReleaseSysCache(dbtup);

	return invalid;
}


/* * recovery_create_dbdir()
 *
 * 在恢复期间，存在一种情况需要恢复缺失的表空间目录
 * 以便恢复可以继续。当恢复想要创建数据库但持有
 * 该数据库的表空间在服务器停止前被删除时，就会
 * 发生这种情况。由于我们预期在达到恢复一致性之前
 * 该目录将消失，并且我们除了其 OID 之外对表空间
 * 一无所知，因此我们在此处在 pg_tblspc 下创建一个
 * 真实目录，而不是恢复符号链接。
 * 如果 only_tblspc 为 true，则请求的目录必须在
 * pg_tblspc/ 下
 
 */
static void
recovery_create_dbdir(char *path, bool only_tblspc)
{
	struct stat st;

	Assert(RecoveryInProgress());

	if (stat(path, &st) == 0)
		return;

	if (only_tblspc && strstr(path, "pg_tblspc/") == NULL)
		elog(PANIC, "requested to created invalid directory: %s", path);

	if (reachedConsistency && !allow_in_place_tablespaces)
		ereport(PANIC,
				errmsg("missing directory \"%s\"", path));

	elog(reachedConsistency ? WARNING : DEBUG1,
		 "creating missing directory: %s", path);

	if (pg_mkdir_p(path, pg_dir_create_mode) != 0)
		ereport(PANIC,
				errmsg("could not create missing directory \"%s\": %m", path));
}


/*
 * DATABASE 资源管理器的例程
 */
void
dbase_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in dbase records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_DBASE_CREATE_FILE_COPY)
	{
		xl_dbase_create_file_copy_rec *xlrec =
			(xl_dbase_create_file_copy_rec *) XLogRecGetData(record);
		char	   *src_path;
		char	   *dst_path;
		char	   *parent_path;
		struct stat st;

		src_path = GetDatabasePath(xlrec->src_db_id, xlrec->src_tablespace_id);
		dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		/*
		 * 我们重放 CREATE 的理论是：如果目标子目录存在，
		 * 则强制删除它，然后重新复制源数据。这可能比
		 * 需要的更繁琐，但实现简单。
		 */
		if (stat(dst_path, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (!rmtree(dst_path, true))
				/* If this failed, copydir() below is going to error. */
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								dst_path)));
		}

		/*
		 * 如果目标路径的父目录不存在，则立即创建它。
		 * 这使我们以后能够在下面创建目标。
		 */
		parent_path = pstrdup(dst_path);
		get_parent_directory(parent_path);
		if (stat(parent_path, &st) < 0)
		{
			if (errno != ENOENT)
				ereport(FATAL,
						errmsg("could not stat directory \"%s\": %m",
							   dst_path));

			/* create the parent directory if needed and valid */
			recovery_create_dbdir(parent_path, true);
		}
		pfree(parent_path);

		/*
		 * 存在一种情况，由于与上面相同的原因，复制源目录
		 * 不存在。创建空的源目录，以便下面的 copydir
		 * 不会失败。该目录将很快被恢复删除。
		 
 */
		if (stat(src_path, &st) < 0 && errno == ENOENT)
			recovery_create_dbdir(src_path, false);

		/*
		 * 强制将脏缓冲区刷新到磁盘，以确保源数据库在复制
		 * 前是最新的。
		 */
		FlushDatabaseBuffers(xlrec->src_db_id);

		/* Close all sgmr fds in all backends. */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		/*
		 * 将此子目录复制到新位置
		 *
		 * 我们不需要复制子目录
		 */
		copydir(src_path, dst_path, false);

		pfree(src_path);
		pfree(dst_path);
	}
	else if (info == XLOG_DBASE_CREATE_WAL_LOG)
	{
		xl_dbase_create_wal_log_rec *xlrec =
			(xl_dbase_create_wal_log_rec *) XLogRecGetData(record);
		char	   *dbpath;
		char	   *parent_path;

		dbpath = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		/* create the parent directory if needed and valid */
		parent_path = pstrdup(dbpath);
		get_parent_directory(parent_path);
		recovery_create_dbdir(parent_path, true);
		pfree(parent_path);

		/* Create the database directory with the version file. */
		CreateDirAndVersionFile(dbpath, xlrec->db_id, xlrec->tablespace_id,
								true);
		pfree(dbpath);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) XLogRecGetData(record);
		char	   *dst_path;
		int			i;

		if (InHotStandby)
		{
			/*
			 * 在解决冲突时锁定数据库，以确保
			 * InitPostgres() 不能完全并发地重新执行。
			 * 这避免了后端在某些情况下自动重新连接
			 * 到同一数据库的问题。
			 *
			 * 这也将锁定尝试连接到数据库特定逻辑解码槽的
			 * walsender，因此我们可以安全地删除槽。
			 
 */
			LockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
			ResolveRecoveryConflictWithDatabase(xlrec->db_id);
		}

		/* Drop any database-specific replication slots */
		ReplicationSlotsDropDBSlots(xlrec->db_id);

		/* Drop pages for this database that are in the shared buffer cache */
		DropDatabaseBuffers(xlrec->db_id);

		/* Also, clean out any fsync requests that might be pending in md.c */
		ForgetDatabaseSyncRequests(xlrec->db_id);

		/* Clean out the xlog relcache too */
		XLogDropDatabase(xlrec->db_id);

		/* Close all sgmr fds in all backends. */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		for (i = 0; i < xlrec->ntablespaces; i++)
		{
			dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_ids[i]);

			/* And remove the physical files */
			if (!rmtree(dst_path, true))
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								dst_path)));
			pfree(dst_path);
		}

		if (InHotStandby)
		{
			/*
			 * 在提交之前释放锁。XXX 这里存在竞争条件，
			 * 可能允许后端重新连接，但此窗口很小，因为
			 * 此处到提交之间的间隙通常相当小，且人们
			 * 不太可能删除我们试图连接的数据库。
			 
 */
			UnlockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
		}
	}
	else
		elog(PANIC, "dbase_redo: unknown op code %u", info);
}
