/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  操作表空间的命令
 *
 * PostgreSQL 中的表空间允许用户决定
 * 给定数据库对象的数据文件在文件系统
 * 中的存储位置。
 *
 * 表空间代表文件系统上的一个目录。在
 * 创建表空间时，该目录必须为空。为了
 * 简化操作并消除文件名冲突的可能性，
 * 我们将表空间中的文件隔离到按数据库划分的子目录中。
 *
 * 为了支持通过 RelFileLocator 中给出的信息
 * 进行文件访问，我们在 $PGDATA/pg_tblspc
 * 中维护了符号链接映射。符号链接以表空间 OIDs
 * 命名并指向实际的表空间目录。每个表空间中
 * 还有按数据库集簇版本划分的目录。因此，任意文件的
 *			$PGDATA/pg_tblspc/spcoid/PG_MAJORVER_CATVER/dboid/relfilenumber
 * 例如
 *			$PGDATA/pg_tblspc/20981/PG_9.0_201002161/719849/83292814
 *
 * initdb 时会创建两个表空间：pg_global
 * （用于共享表）和 pg_default（用于其他
 * 所有内容）。为了向后兼容并确保在不支持
 * 符号链接的平台上仍能正常工作，这两个表空间
 *			$PGDATA/global/relfilenumber
 *			$PGDATA/base/dboid/relfilenumber
 *
 * 为了支持 CREATE DATABASE 为新数据库指定
 * 与模板数据库默认值不同的默认表空间，
 * 我们约定 pg_class.reltablespace 中的
 * 零值表示数据库的默认表空间。如果没有
 * 这个约定，CREATE DATABASE 就必须修改新数据库的系统目录。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/standby.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

/* GUC 变量 */
char	   *default_tablespace = NULL;
char	   *temp_tablespaces = NULL;
bool		allow_in_place_tablespaces = false;

Oid			binary_upgrade_next_pg_tablespace_oid = InvalidOid;

static void create_tablespace_directories(const char *location,
										  const Oid tablespaceoid);
static bool destroy_tablespace_directories(Oid tablespaceoid, bool redo);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * If tablespaces are not supported, we still need it in case we have to
 * re-create a database subdirectory (of $PGDATA/base) during WAL replay.
 */
void
TablespaceCreateDbspace(Oid spcOid, Oid dbOid, bool isRedo)
{
	struct stat st;
	char	   *dir;

	/*
	 * 全局表空间没有按数据库划分的子目录，因此
	 * 无需任何操作。
	 */
	if (spcOid == GLOBALTABLESPACE_OID)
		return;

	Assert(OidIsValid(spcOid));
	Assert(OidIsValid(dbOid));

	dir = GetDatabasePath(dbOid, spcOid);

	if (stat(dir, &st) < 0)
	{
		/* 目录不存在？ */
		if (errno == ENOENT)
		{
			/*
			 * 获取 TablespaceCreateLock 以确保没有并发的
			 * DROP TABLESPACE 或 TablespaceCreateDbspace 操作。
			 */
			LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

			/*
			 * 重新检查是否有人在我们等待锁期间
			 * 创建了目录。
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* 目录已创建 */
			}
			else
			{
				/* 目录创建失败？ */
				if (MakePGDirectory(dir) < 0)
				{
					/* 失败原因不是不存在或不在 WAL 重放中？ */
					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));

					/*
					 * 在 WAL 重放期间，如果表空间在 WAL
					 * 流中的删除位置比当前重放位置更靠前，
					 * 则可能出现多层目录缺失的情况。
					 * 一个简单的解决方案是将它们创建为普通目录，
					 * 并希望后续的 WAL 重放能在必要时
					 * 删除它们。如果这也失败了，
					 * 那就是无法摆脱的困境，因此只能报告
					 * 该情况并终止。
					 */
					if (pg_mkdir_p(dir, pg_dir_create_mode) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));
				}
			}

			LWLockRelease(TablespaceCreateLock);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else
	{
		/* 它不是目录？ */
		if (!S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" exists but is not a directory",
							dir)));
	}

	pfree(dir);
}

/*
 * 创建表空间
 *
 * 仅超级用户可以创建表空间。这是一个合理的限制，
 * 因为我们正在决定系统布局，而且无论如何，
 * 执行此类操作的用户通常拥有 root 权限。
 */
Oid
CreateTableSpace(CreateTableSpaceStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_tablespace];
	bool		nulls[Natts_pg_tablespace] = {0};
	HeapTuple	tuple;
	Oid			tablespaceoid;
	char	   *location;
	Oid			ownerId;
	Datum		newOptions;
	bool		in_place;

	/* 必须是超级用户 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create tablespace \"%s\"",
						stmt->tablespacename),
				 errhint("Must be superuser to create a tablespace.")));

	/* 但是，表空间的最终所有者可以不是超级用户 */
	if (stmt->owner)
		ownerId = get_rolespec_oid(stmt->owner, false);
	else
		ownerId = GetUserId();

	/* 将路径转换为 Unix 格式，并去除尾部斜杠 */
	location = pstrdup(stmt->location);
	canonicalize_path(location);

	/* 禁止使用引号，否则 CREATE DATABASE 会有风险 */
	if (strchr(location, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("tablespace location cannot contain single quotes")));

	in_place = allow_in_place_tablespaces && strlen(location) == 0;

	/*
	 * 允许相对路径似乎有风险
	 *
	 * 这也有助于我们确保位置不为空或不为空白，
	 * 除非指定的是仅限开发者的就地表空间。
	 */
	if (!in_place && !is_absolute_path(location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location must be an absolute path")));

	/*
	 * 检查位置是否过长。请注意，我们还要追加
	 * 'PG_XXX/<dboid>/<relid>_<fork>.<nnn>'。
	 * 顺带一提，我们这里实际上从未引用完整路径，
	 * 但 MakePGDirectory() 使用了前两部分。
	 */
	if (strlen(location) + 1 + strlen(TABLESPACE_VERSION_DIRECTORY) + 1 +
		OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1 + OIDCHARS > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long",
						location)));

	/* 如果表空间位于数据目录中则发出警告。 */
	if (path_is_prefix_of_path(DataDir, location))
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location should not be inside the data directory")));

	/*
	 * 禁止创建名为 "pg_xxx" 的表空间；我们将此
	 * 命名空间保留用于系统目的。
	 */
	if (!allowSystemTableMods && IsReservedName(stmt->tablespacename))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"",
						stmt->tablespacename),
				 errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * 如果构建时启用了相应开关，则在违反回归测试
	 * 中表空间命名约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->tablespacename, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/*
	 * 检查是否已存在同名的其他表空间。（唯一索引
	 * 本就会捕捉到此情况，但提供更友好的错误消息
	 * 也无妨。）
	 */
	if (OidIsValid(get_tablespace_oid(stmt->tablespacename, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						stmt->tablespacename)));

	/*
	 * 将元组插入 pg_tablespace。首先执行此操作
	 * 是为了锁定提议的表名以防止其他创建者。
	 * 如果后续发现问题，插入操作将会回滚。
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	if (IsBinaryUpgrade)
	{
		/* 使用 binary-upgrade 覆盖表空间的 OID */
		if (!OidIsValid(binary_upgrade_next_pg_tablespace_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_tablespace OID value not set when in binary upgrade mode")));

		tablespaceoid = binary_upgrade_next_pg_tablespace_oid;
		binary_upgrade_next_pg_tablespace_oid = InvalidOid;
	}
	else
		tablespaceoid = GetNewOidWithIndex(rel, TablespaceOidIndexId,
										   Anum_pg_tablespace_oid);
	values[Anum_pg_tablespace_oid - 1] = ObjectIdGetDatum(tablespaceoid);
	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		ObjectIdGetDatum(ownerId);
	nulls[Anum_pg_tablespace_spcacl - 1] = true;

	/* 生成新的建议 spcoptions（文本数组） */
	newOptions = transformRelOptions((Datum) 0,
									 stmt->options,
									 NULL, NULL, false, false);
	(void) tablespace_reloptions(newOptions, true);
	if (newOptions != (Datum) 0)
		values[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		nulls[Anum_pg_tablespace_spcoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* 记录对所有者依赖 */
	recordDependencyOnOwner(TableSpaceRelationId, tablespaceoid, ownerId);

	/* 新表空间的创建后钩子 */
	InvokeObjectPostCreateHook(TableSpaceRelationId, tablespaceoid, 0);

	create_tablespace_directories(location, tablespaceoid);

	/* 在 XLOG 中记录文件系统更改 */
	{
		xl_tblspc_create_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec,
						 offsetof(xl_tblspc_create_rec, ts_path));
		XLogRegisterData((char *) location, strlen(location) + 1);

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_CREATE);
	}

	/*
	 * 强制同步提交，以最小化创建磁盘上的符号链接
	 * 与标记事务已提交之间的时间窗口。存在任何
	 * 窗口期都不理想，但我们绝不想使其超过
	 * 必要的大小。
	 */
	ForceSyncCommit();

	pfree(location);

	/* 我们保持 pg_tablespace 上的锁直到提交 */
	table_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * 删除表空间
 *
 * 请注意检查表空间是否为空。
 */
void
DropTableSpace(DropTableSpaceStmt *stmt)
{
	char	   *tablespacename = stmt->tablespacename;
	TableScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_tablespace spcform;
	ScanKeyData entry[1];
	Oid			tablespaceoid;
	char	   *detail;
	char	   *detail_log;

	/*
	 * 查找目标元组
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("tablespace \"%s\" does not exist, skipping",
							tablespacename)));
			table_endscan(scandesc);
			table_close(rel, NoLock);
		}
		return;
	}

	spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
	tablespaceoid = spcform->oid;

	/* 必须是表空间所有者 */
	if (!object_ownercheck(TableSpaceRelationId, tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   tablespacename);

	/* 禁止删除标准表空间，即使是超级用户也不行 */
	if (IsPinnedObject(TableSpaceRelationId, tablespaceoid))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE,
					   tablespacename);

	/* 检查是否有依赖于此表空间的 pg_shdepend 条目 */
	if (checkSharedDependencies(TableSpaceRelationId, tablespaceoid,
								&detail, &detail_log))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("tablespace \"%s\" cannot be dropped because some objects depend on it",
						tablespacename),
				 errdetail_internal("%s", detail),
				 errdetail_log("%s", detail_log)));

	/* 被删除表空间的 DROP 钩子 */
	InvokeObjectDropHook(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * 删除 pg_tablespace 元组（如果后续失败，此操作将回滚）
	 */
	CatalogTupleDelete(rel, &tuple->t_self);

	table_endscan(scandesc);

	/*
	 * 删除该表空间上的所有注释或安全标签。
	 */
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);
	DeleteSharedSecurityLabel(tablespaceoid, TableSpaceRelationId);

	/*
	 * 删除对所有者依赖的记录。
	 */
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * 获取 TablespaceCreateLock 以确保没有并发的
	 * TablespaceCreateDbspace 调用。
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/*
	 * 尝试删除物理基础设施。
	 */
	if (!destroy_tablespace_directories(tablespaceoid, false))
	{
		/*
		 * 并非所有文件都已删除？然而，目录中可能存在
		 * 残留的空文件，这些文件可能是 DROP TABLE 等
		 * 操作留下的，已被安排在下一个检查点时删除
		 * （详见 mdunlink() 中的注释）。我们可以立即
		 * 删除它们，但无法将它们与不能删除的重要
		 * 数据文件区分开来。因此，我们强制执行一个
		 * 检查点来清理所有残留文件，然后重试。
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * 在 Windows 上，已解除链接的文件会一直保留
		 * 在目录列表中，直到没有进程持有该文件的
		 * 打开句柄。安排文件取消链接的 DDL 命令会
		 * 发送失效消息，指示其他 PostgreSQL 进程
		 * 关闭这些文件，但无法保证它们会及时处理。
		 * 因此，我们还将使用全局屏障要求所有后端
		 * 进程关闭所有文件，并等待它们完成。
		 */
		LWLockRelease(TablespaceCreateLock);
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));
		LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

		/* 现在重试。 */
		if (!destroy_tablespace_directories(tablespaceoid, false))
		{
			/* 仍然非空，那这些文件一定是重要的 */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));
		}
	}

	/* 在 XLOG 中记录文件系统更改 */
	{
		xl_tblspc_drop_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_tblspc_drop_rec));

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP);
	}

	/*
	 * 注意：由于我们已检查表空间为空，因此
	 * 无需担心刷新表空间中关系的共享缓冲区
	 * 或空闲空间映射条目。
	 */

	/*
	 * 强制同步提交，以最小化删除磁盘上的文件
	 * 与标记事务已提交之间的时间窗口。存在任何
	 * 窗口期都不理想，但我们绝不想使其超过
	 * 必要的大小。
	 */
	ForceSyncCommit();

	/*
	 * 再次允许 TablespaceCreateDbspace。
	 */
	LWLockRelease(TablespaceCreateLock);

	/* 我们保持 pg_tablespace 上的锁直到提交 */
	table_close(rel, NoLock);
}


/*
 * create_tablespace_directories
 *
 *	尝试创建将 $PGDATA/pg_tblspc/ 链接到
 *	指定目录的文件系统基础设施
 */
static void
create_tablespace_directories(const char *location, const Oid tablespaceoid)
{
	char	   *linkloc;
	char	   *location_with_version_dir;
	struct stat st;
	bool		in_place;

	linkloc = psprintf("pg_tblspc/%u", tablespaceoid);

	/*
	 * 如果要求创建'就地'表空间，则直接在符号链接
	 * 通常所在的位置创建目录。目前这是一个仅限
	 * 开发者使用的选项，用于简化回归测试。
	 */
	in_place = strlen(location) == 0;

	if (in_place)
	{
		if (MakePGDirectory(linkloc) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							linkloc)));
	}

	location_with_version_dir = psprintf("%s/%s", in_place ? linkloc : location,
										 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * 尝试将目标目录设置为安全权限。如果失败，
	 * 说明目录不存在或所有者错误。就地模式下
	 * 不需要此步骤，因为在这种情况下我们创建
	 * 目录时已使用了所需的权限。
	 */
	if (!in_place && chmod(location, pg_dir_create_mode) != 0)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FILE),
					 errmsg("directory \"%s\" does not exist", location),
					 InRecovery ? errhint("Create this directory for the tablespace before "
										  "restarting the server.") : 0));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not set permissions on directory \"%s\": %m",
							location)));
	}

	/*
	 * 版本目录的创建可防止在同一位置存在
	 * 多个表空间。这模仿了 TablespaceCreateDbspace()，
	 * 但它忽略了并发性和缺失父目录的情况。
	 * 如果缺少父目录，chmod() 会失败。
	 * pg_tablespace_spcname_index 可防止并发问题。
	 */
	if (stat(location_with_version_dir, &st) < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m",
							location_with_version_dir)));
		else if (MakePGDirectory(location_with_version_dir) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							location_with_version_dir)));
	}
	else if (!S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" exists but is not a directory",
						location_with_version_dir)));
	else if (!InRecovery)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("directory \"%s\" already in use as a tablespace",
						location_with_version_dir)));

	/*
	 * 在恢复期间，删除旧的符号链接，以防它指向
	 */
	if (!in_place && InRecovery)
		remove_tablespace_symlink(linkloc);

	/*
	 * 在 PGDATA 下创建符号链接
	 */
	if (!in_place && symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	pfree(linkloc);
	pfree(location_with_version_dir);
}


/*
 * destroy_tablespace_directories
 *
 * 尝试删除表空间的文件系统基础设施。
 *
 * 'redo' 表示我们正在从 XLOG 重放删除操作；
 * 在这种情况下，出错时不应抛出 ERROR，只需
 * LOG 即可。此处未能删除文件的最严重后果
 * 是未能释放部分磁盘空间，这不足以证明
 * 抛出需要手动干预才能让数据库重新运行的错误是合理的。
 *
 * 成功返回 true，如果某些子目录非空则返回 false
 */
static bool
destroy_tablespace_directories(Oid tablespaceoid, bool redo)
{
	char	   *linkloc;
	char	   *linkloc_with_version_dir;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;
	struct stat st;

	linkloc_with_version_dir = psprintf("pg_tblspc/%u/%s", tablespaceoid,
										TABLESPACE_VERSION_DIRECTORY);

	/*
	 * 检查表空间是否仍包含任何文件。我们尝试 rmdir
	 * 其中的每个按数据库划分的子目录。rmdir 失败
	 * 意味着该子目录中仍有文件，因此放弃。
	 * （我们不必担心撤销任何已完成的 rmdir，
	 * 因为下次使用该数据库的表空间时会通过
	 * TablespaceCreateDbspace 重新创建子目录。）
	 *
	 * 由于我们持有 TablespaceCreateLock，其他人不
	 * 应并行创建任何新的子目录。不过，新文件
	 * 可能仍在子目录中被创建，因此 rmdir 调用
	 * 可能失败。最坏的结果是收到不太友好的错误消息。
	 *
	 * 如果 redo 为 true，则 ENOENT 可能是预期的
	 * 结果，我们允许它静默通过。在正常操作中
	 * 我们仍然允许它，但会发出警告。这是因为即使
	 * ProcessUtility 不允许在事务块中使用 DROP
	 * TABLESPACE，之前的 DROP 可能在删除表空间
	 * 目录和/或符号链接后失败并回滚。我们想要
	 * 允许新的 DROP 尝试成功删除目录条目
	 * （和符号链接，如果仍存在的话），因此不应在此处给出硬错误。
	 */
	dirdesc = AllocateDir(linkloc_with_version_dir);
	if (dirdesc == NULL)
	{
		if (errno == ENOENT)
		{
			if (!redo)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m",
								linkloc_with_version_dir)));
			/* 符号链接可能仍然存在，尝试将其删除 */
			goto remove_symlink;
		}
		else if (redo)
		{
			/* 在 redo 中，仅记录其他类型的错误 */
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							linkloc_with_version_dir)));
			pfree(linkloc_with_version_dir);
			return false;
		}
		/* 否则让 ReadDir 报告错误 */
	}

	while ((de = ReadDir(dirdesc, linkloc_with_version_dir)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		subfile = psprintf("%s/%s", linkloc_with_version_dir, de->d_name);

		/* 此检查只是为了提供更友好的错误消息 */
		if (!redo && !directory_is_empty(subfile))
		{
			FreeDir(dirdesc);
			pfree(subfile);
			pfree(linkloc_with_version_dir);
			return false;
		}

		/* 删除空目录 */
		if (rmdir(subfile) < 0)
			ereport(redo ? LOG : ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}

	FreeDir(dirdesc);

	/* 删除版本目录 */
	if (rmdir(linkloc_with_version_dir) < 0)
	{
		ereport(redo ? LOG : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m",
						linkloc_with_version_dir)));
		pfree(linkloc_with_version_dir);
		return false;
	}

	/*
	 * 尝试删除符号链接。然而我们必须处理它可能是
	 * 目录而非符号链接的情况 --- 这种情况可能发生在
	 * WAL 重放期间（参见 TablespaceCreateDbspace）。
	 *
	 * 注意：在 redo 情况下，如果最后一步失败，
	 * 我们将返回 true；重试没有意义。此外，ENOENT
	 * 不应触发超过警告级别的响应。
	 */
remove_symlink:
	linkloc = pstrdup(linkloc_with_version_dir);
	get_parent_directory(linkloc);
	if (lstat(linkloc, &st) < 0)
	{
		int			saved_errno = errno;

		ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						linkloc)));
	}
	else if (S_ISDIR(st.st_mode))
	{
		if (rmdir(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
		}
	}
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
		}
	}
	else
	{
		/* 拒绝删除任何非目录或符号链接的内容 */
		ereport(redo ? LOG : ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}

	pfree(linkloc_with_version_dir);
	pfree(linkloc);

	return true;
}


/*
 * 检查目录是否为空。
 *
 * 这个函数可能应该放在其他地方，但不确定放在哪里...
 */
bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);

	while ((de = ReadDir(dirdesc, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		FreeDir(dirdesc);
		return false;
	}

	FreeDir(dirdesc);
	return true;
}

/*
 *	remove_tablespace_symlink
 *
 * 此函数删除 pg_tblspc 中的符号链接。在 Windows
 * 上，junction points 的行为类似于目录，因此我们必须
 * 能够使用 rmdir。与 destroy_tablespace_directories 中的
 * tablespace_directories 中的符号链接删除代码
 * 类似，区别仅在于删除失败时始终是 ERROR。但如果文件根本不存在，则没有问题。
 */
void
remove_tablespace_symlink(const char *linkloc)
{
	struct stat st;

	if (lstat(linkloc, &st) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", linkloc)));
	}

	if (S_ISDIR(st.st_mode))
	{
		/*
		 * 如果目录非空则会失败，
		 * 但如果是 junction point 则不会。
		 */
		if (rmdir(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
	}
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
	}
	else
	{
		/* 拒绝删除任何非目录或符号链接的内容 */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}
}

/*
 * 重命名表空间
 */
ObjectAddress
RenameTableSpace(const char *oldname, const char *newname)
{
	Oid			tspId;
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	newtuple;
	Form_pg_tablespace newform;
	ObjectAddress address;

	/* 搜索 pg_tablespace */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						oldname)));

	newtuple = heap_copytuple(tup);
	newform = (Form_pg_tablespace) GETSTRUCT(newtuple);
	tspId = newform->oid;

	table_endscan(scan);

	/* 必须是所有者 */
	if (!object_ownercheck(TableSpaceRelationId, tspId, GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, oldname);

	/* 验证新名称 */
	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"", newname),
				 errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * 如果构建时启用了相应开关，则在违反回归测试
	 * 中表空间命名约定时发出警告。
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(newname, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/* 确保新名称不存在 */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						newname)));

	table_endscan(scan);

	/* 好，更新条目 */
	namestrcpy(&(newform->spcname), newname);

	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tspId, 0);

	ObjectAddressSet(address, TableSpaceRelationId, tspId);

	table_close(rel, NoLock);

	return address;
}

/*
 * 修改表空间选项
 */
Oid
AlterTableSpaceOptions(AlterTableSpaceOptionsStmt *stmt)
{
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scandesc;
	HeapTuple	tup;
	Oid			tablespaceoid;
	Datum		datum;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_tablespace];
	bool		isnull;
	bool		repl_null[Natts_pg_tablespace];
	bool		repl_repl[Natts_pg_tablespace];
	HeapTuple	newtuple;

	/* 搜索 pg_tablespace */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						stmt->tablespacename)));

	tablespaceoid = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

	/* 必须是现有对象的所有者 */
	if (!object_ownercheck(TableSpaceRelationId, tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   stmt->tablespacename);

	/* 生成新的建议 spcoptions（文本数组） */
	datum = heap_getattr(tup, Anum_pg_tablespace_spcoptions,
						 RelationGetDescr(rel), &isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 stmt->options, NULL, NULL, false,
									 stmt->isReset);
	(void) tablespace_reloptions(newOptions, true);

	/* 构建新元组。 */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_tablespace_spcoptions - 1] = true;
	repl_repl[Anum_pg_tablespace_spcoptions - 1] = true;
	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val,
								 repl_null, repl_repl);

	/* 更新系统目录。 */
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tablespaceoid, 0);

	heap_freetuple(newtuple);

	/* 结束堆扫描。 */
	table_endscan(scandesc);
	table_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * 处理 GUC 变量 'default_tablespace' 的例程。
 */

/* check_hook: 验证新的 default_tablespace */
bool
check_default_tablespace(char **newval, void **extra, GucSource source)
{
	/*
	 * 如果我们不在事务内，或者未连接到数据库，
	 * 则无法进行验证名称所需的目录访问。
	 * 此时只能接受该值。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		if (**newval != '\0' &&
			!OidIsValid(get_tablespace_oid(*newval, true)))
		{
			/*
			 * 当 source == PGC_S_TEST 时，对不存在的
			 * 表空间不抛出硬错误，仅给出 NOTICE。参见 guc.h 中的注释。
			 */
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tablespace \"%s\" does not exist",
								*newval)));
			}
			else
			{
				GUC_check_errdetail("Tablespace \"%s\" does not exist.",
									*newval);
				return false;
			}
		}
	}

	return true;
}

/*
 * GetDefaultTablespace -- 获取当前默认表空间的 OID
 *
 * 临时对象有不同的默认表空间，因此必须
 * 指定 relpersistence 参数。此外，对于分区表，
 * 我们禁止指定数据库默认值，因此这也需要
 * 指定。
 *
 * 可能返回 InvalidOid 以表示"使用数据库的默认表空间"。
 *
 * 注意：调用者应检查除 InvalidOid 之外的
 * 任何结果的相应权限。
 *
 * 此函数的存在是为了隐藏（并可能优化使用）
 * default_tablespace GUC 变量。
 */
Oid
GetDefaultTablespace(char relpersistence, bool partitioned)
{
	Oid			result;

	/* 临时表的情况在其他地方处理 */
	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		PrepareTempTablespaces();
		return GetNextTempTableSpace();
	}

	/* default_tablespace == "" 的快速路径 */
	if (default_tablespace == NULL || default_tablespace[0] == '\0')
		return InvalidOid;

	/*
	 * 缓存此查找以获得更快速度很有诱惑力，但那样
	 * 会无法检测到 GUC 变量设置后表空间被删除的
	 * 情况。另请注意，如果值未能引用现有表空间，
	 * 我们不会报错；只是静默返回 InvalidOid，
	 * 使新对象在数据库的表空间中创建。
	 */
	result = get_tablespace_oid(default_tablespace, true);

	/*
	 * 允许在 default_tablespace 中显式指定数据库
	 * 的默认表空间，而无需触发权限检查。但在
	 * 创建分区表时不允许这样做，因为结果会
	 * 令人困惑。
	 */
	if (result == MyDatabaseTableSpace)
	{
		if (partitioned)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
		result = InvalidOid;
	}
	return result;
}


/*
 * 处理 GUC 变量 'temp_tablespaces' 的例程。
 */

typedef struct
{
	/* 要传递给 SetTempTablespaces() 的 OIDs 数组 */
	int			numSpcs;
	Oid			tblSpcs[FLEXIBLE_ARRAY_MEMBER];
} temp_tablespaces_extra;

/* check_hook: 验证新的 temp_tablespaces */
bool
check_temp_tablespaces(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	List	   *namelist;

	/* 需要字符串的可修改副本 */
	rawname = pstrdup(*newval);

	/* 将字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* 名称列表中的语法错误 */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}

	/*
	 * 如果我们不在事务内，或者未连接到数据库，
	 * 则无法进行验证名称所需的目录访问。
	 * 此时只能接受该值。幸运的是，此时也
	 * 不需要将数据传递给 fd.c。
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		temp_tablespaces_extra *myextra;
		Oid		   *tblSpcs;
		int			numSpcs;
		ListCell   *l;

		/* 临时工作空间，直到我们完成列表验证 */
		tblSpcs = (Oid *) palloc(list_length(namelist) * sizeof(Oid));
		numSpcs = 0;
		foreach(l, namelist)
		{
			char	   *curname = (char *) lfirst(l);
			Oid			curoid;
			AclResult	aclresult;

			/* 允许空字符串（表示数据库默认值） */
			if (curname[0] == '\0')
			{
				/* InvalidOid 表示数据库的默认表空间 */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/*
			 * 在交互式 SET 命令中，我们使用 ereport
			 * 处理错误信息。当 source == PGC_S_TEST
			 * 时，对不存在的表空间仅给出 NOTICE。参见 guc.h 中的注释。
			 */
			curoid = get_tablespace_oid(curname, source <= PGC_S_TEST);
			if (curoid == InvalidOid)
			{
				if (source == PGC_S_TEST)
					ereport(NOTICE,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace \"%s\" does not exist",
									curname)));
				continue;
			}

			/*
			 * 允许在 temp_tablespaces 中显式指定
			 * 数据库的默认表空间，而无需触发权限检查。
			 */
			if (curoid == MyDatabaseTableSpace)
			{
				/* InvalidOid 表示数据库的默认表空间 */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/* 检查权限，类似地仅在交互式情况下才报错 */
			aclresult = object_aclcheck(TableSpaceRelationId, curoid, GetUserId(),
										ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
			{
				if (source >= PGC_S_INTERACTIVE)
					aclcheck_error(aclresult, OBJECT_TABLESPACE, curname);
				continue;
			}

			tblSpcs[numSpcs++] = curoid;
		}

		/* 现在为 assign_temp_tablespaces 准备 "extra" 结构体 */
		myextra = guc_malloc(LOG, offsetof(temp_tablespaces_extra, tblSpcs) +
							 numSpcs * sizeof(Oid));
		if (!myextra)
			return false;
		myextra->numSpcs = numSpcs;
		memcpy(myextra->tblSpcs, tblSpcs, numSpcs * sizeof(Oid));
		*extra = (void *) myextra;

		pfree(tblSpcs);
	}

	pfree(rawname);
	list_free(namelist);

	return true;
}

/* assign_hook: 根据需要执行额外操作 */
void
assign_temp_tablespaces(const char *newval, void *extra)
{
	temp_tablespaces_extra *myextra = (temp_tablespaces_extra *) extra;

	/*
	 * 如果 check_temp_tablespaces 在事务内执行，
	 * 则将其生成的列表传递给 fd.c。否则，清除
	 * fd.c 的列表；我们必须仍在事务之外，或者正在
	 * 事务退出时恢复中，无论哪种情况，都可以
	 * 让下一次 PrepareTempTablespaces 调用来恢复正常。
	 */
	if (myextra)
		SetTempTablespaces(myextra->tblSpcs, myextra->numSpcs);
	else
		SetTempTablespaces(NULL, 0);
}

/*
 * PrepareTempTablespaces -- 准备使用临时表空间
 *
 * 如果我们在当前事务中尚未执行此操作，
 * 则解析 temp_tablespaces GUC 变量并告知 fd.c 使用哪些 tablespace
 * fd.c 将哪些表空间用于临时文件。
 */
void
PrepareTempTablespaces(void)
{
	char	   *rawname;
	List	   *namelist;
	Oid		   *tblSpcs;
	int			numSpcs;
	ListCell   *l;

	/* 如果在当前事务中已完成则无需操作 */
	if (TempTablespacesAreSet())
		return;

	/*
	 * 除非在事务内，否则无法进行目录访问。
	 * 这只是一个安全检查，以防此函数被可能在
	 * 事务外执行的低级代码调用。注意，在此
	 * 场景下，fd.c 将回退到使用当前数据库的
	 * 默认表空间，这应该总是安全的。
	 */
	if (!IsTransactionState())
		return;

	/* 需要字符串的可修改副本 */
	rawname = pstrdup(temp_tablespaces);

	/* 将字符串解析为标识符列表 */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* 名称列表中的语法错误 */
		SetTempTablespaces(NULL, 0);
		pfree(rawname);
		list_free(namelist);
		return;
	}

	/* 将表空间 OIDs 存储在 TopTransactionContext 的数组中 */
	tblSpcs = (Oid *) MemoryContextAlloc(TopTransactionContext,
										 list_length(namelist) * sizeof(Oid));
	numSpcs = 0;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			curoid;
		AclResult	aclresult;

		/* 允许空字符串（表示数据库默认值） */
		if (curname[0] == '\0')
		{
			/* InvalidOid 表示数据库的默认表空间 */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* 否则验证名称是否为有效的表空间名称 */
		curoid = get_tablespace_oid(curname, true);
		if (curoid == InvalidOid)
		{
			/* 跳过任何无效的列表元素 */
			continue;
		}

		/*
		 * 允许在 temp_tablespaces 中显式指定
		 * 数据库的默认表空间，而无需触发权限检查。
		 */
		if (curoid == MyDatabaseTableSpace)
		{
			/* InvalidOid 表示数据库的默认表空间 */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* 类似地检查权限 */
		aclresult = object_aclcheck(TableSpaceRelationId, curoid, GetUserId(),
									ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			continue;

		tblSpcs[numSpcs++] = curoid;
	}

	SetTempTablespaces(tblSpcs, numSpcs);

	pfree(rawname);
	list_free(namelist);
}


/*
 * get_tablespace_oid - 给定表空间名称，查找其 OID
 *
 * 如果 missing_ok 为 false，在找不到表空间名称时
 * 抛出错误。如果为 true，则仅返回 InvalidOid。
 */
Oid
get_tablespace_oid(const char *tablespacename, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * 搜索 pg_tablespace。我们在这里使用堆扫描而非
	 * 索引扫描，尽管存在名称上的索引，但理论是
	 * pg_tablespace 通常只有少量条目，因此索引查找是浪费精力。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* 我们假设最多只有一个匹配的元组 */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_tablespace) GETSTRUCT(tuple))->oid;
	else
		result = InvalidOid;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						tablespacename)));

	return result;
}

/*
 * get_tablespace_name - 给定表空间 OID，查找其名称
 *
 * 返回一个 palloc 分配的字符串，如果不存在该表空间则返回 NULL。
 */
char *
get_tablespace_name(Oid spc_oid)
{
	char	   *result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * 搜索 pg_tablespace。我们在这里使用堆扫描而非
	 * 索引扫描，尽管存在 OID 上的索引，但理论是
	 * pg_tablespace 通常只有少量条目，因此索引查找是浪费精力。
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* 我们假设最多只有一个匹配的元组 */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	return result;
}


/*
 * TABLESPACE 资源管理器的例程
 */
void
tblspc_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* 备份块不使用 tblspc 记录 */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) XLogRecGetData(record);
		char	   *location = xlrec->ts_path;

		create_tablespace_directories(location, xlrec->ts_id);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

		/* 在所有后端中关闭所有 smgr 文件描述符。 */
		WaitForProcSignalBarrier(EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

		/*
		 * 如果我们为删除表空间发出了 WAL 记录，
		 * 这意味着在执行 DROP 时其中没有任何文件。
		 * 这意味着此时其中不可能存在任何永久对象。
		 *
		 * 备用服务器上的用户可能将此表空间用作其
		 * 临时文件的位置，因此如果我们未能删除所有
		 * 文件，则执行冲突处理并重试（如果当前已
		 * 启用）。
		 *
		 * 其他可能的失败原因包括备用服务器上的文件
		 * 权限被搞乱，而主服务器上一切正常等等。
		 * 对此我们能做的有限，因此只能尽量删除
		 * 并继续。
		 */
		if (!destroy_tablespace_directories(xlrec->ts_id, true))
		{
			ResolveRecoveryConflictWithTablespace(xlrec->ts_id);

			/*
			 * 如果我们进行了恢复处理，那么希望写入
			 * 临时文件的后端进程此时应该已经清理并退出。
			 * 因此在报错前重试。如果再次失败，这只是
			 * 一个 LOG 条件，因为不值得为此抛出 ERROR
			 * （因为那会导致数据库崩溃，并且在重启时
			 * 需要通过手动干预才能越过此 WAL 记录）。
			 */
			if (!destroy_tablespace_directories(xlrec->ts_id, true))
				ereport(LOG,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("directories for tablespace %u could not be removed",
								xlrec->ts_id),
						 errhint("You can remove the directories manually if necessary.")));
		}
	}
	else
		elog(PANIC, "tblspc_redo: unknown op code %u", info);
}
