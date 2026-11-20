/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  虚拟文件描述符定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fd.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * calls:
 *
 *	File{Close/Read/Write/Size/Sync}
 *	{PathNameOpen/Allocate/Free}File
 *
 * 这些绝非 UNIX 例程的简单更名。
 * 所有文件活动均用之...
 *
 *	File FD;
 *	FD = PathNameOpenFile("foo", O_RDONLY);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * 若需 stdio 文件 (FILE*)，用 AllocateFile 而非 fopen；
 * 用 FreeFile 而非 fclose 关闭。切勿对打算长时间
 * 打开的文件用 stdio，因其无法与其他文件
 * 共享内核文件描述符。
 *
 * 同样，用 AllocateDir/FreeDir 而非 opendir/closedir 分配
 * 打开的目录 (DIR*)，且 OpenTransientFile/CloseTransientFile 用于
 * 无缓冲描述符。
 *
 * 若确实无法用上述任一，至少调用 AcquireExternalFD
 * 或 ReserveExternalFD 报告任何长时间持有的
 * 文件描述符。否则可能引发不必要的 EMFILE 错误。
 */
#ifndef FD_H
#define FD_H

#include <dirent.h>
#include <fcntl.h>

typedef enum RecoveryInitSyncMethod
{
	RECOVERY_INIT_SYNC_METHOD_FSYNC,
	RECOVERY_INIT_SYNC_METHOD_SYNCFS
}			RecoveryInitSyncMethod;

typedef int File;


#define IO_DIRECT_DATA			0x01
#define IO_DIRECT_WAL			0x02
#define IO_DIRECT_WAL_INIT		0x04


/* GUC 参数 */
extern PGDLLIMPORT int max_files_per_process;
extern PGDLLIMPORT bool data_sync_retry;
extern PGDLLIMPORT int recovery_init_sync_method;
extern PGDLLIMPORT int io_direct_flags;

/*
 * 本项为 fd.c 私有，但为 save/restore_backend_variables() 导出
 */
extern PGDLLIMPORT int max_safe_fds;

/*
 * 在 Windows 上，必须把 EACCES 也可能理解为与
 * ENOENT 相同，因为该平台上文件已 unlink 但尚未
 * 消失时就会得到此错误。唉。本代码设计为：若无
 * 进一步证据（即待处理的 fsync 请求被取消……见
 * ProcessSyncRequests），并不真正相信这些情况无妨。
 */
#ifndef WIN32
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT)
#else
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
#endif

/*
 * O_DIRECT 并非标准，但几乎每个 Unix 都有。我们
 * 在 src/port/open.c 把它译为相应 Windows 标志。在
 * macOS 上于 fd.c 的 open() 包装内用 fcntl(F_NOCACHE)
 * 模拟。该情况用名 PG_O_DIRECT 而非定义 O_DIRECT（在
 * Unix 上大概不是好主意）。仅当编译器能正确对齐
 * PGIOAlignedBlock 时才能用。
 */
#if defined(O_DIRECT) && defined(pg_attribute_aligned)
#define		PG_O_DIRECT O_DIRECT
#elif defined(F_NOCACHE)
#define		PG_O_DIRECT 0x80000000
#define		PG_O_DIRECT_USE_F_NOCACHE
#else
#define		PG_O_DIRECT 0
#endif

/*
 * fd.c 中函数原型
 */

/* 虚拟 File 上的操作 --- 相当于 Unix 内核文件操作 */
extern File PathNameOpenFile(const char *fileName, int fileFlags);
extern File PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern File OpenTemporaryFile(bool interXact);
extern void FileClose(File file);
extern int	FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern int	FileRead(File file, void *buffer, size_t amount, off_t offset, uint32 wait_event_info);
extern int	FileWrite(File file, const void *buffer, size_t amount, off_t offset, uint32 wait_event_info);
extern int	FileSync(File file, uint32 wait_event_info);
extern int	FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern int	FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info);

extern off_t FileSize(File file);
extern int	FileTruncate(File file, off_t offset, uint32 wait_event_info);
extern void FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info);
extern char *FilePathName(File file);
extern int	FileGetRawDesc(File file);
extern int	FileGetRawFlags(File file);
extern mode_t FileGetRawMode(File file);

/* 用于共享具名临时文件的操作 */
extern File PathNameCreateTemporaryFile(const char *path, bool error_on_failure);
extern File PathNameOpenTemporaryFile(const char *path, int mode);
extern bool PathNameDeleteTemporaryFile(const char *path, bool error_on_failure);
extern void PathNameCreateTemporaryDir(const char *basedir, const char *directory);
extern void PathNameDeleteTemporaryDir(const char *dirname);
extern void TempTablespacePath(char *path, Oid tablespace);

/* 允许使用常规 stdio 的操作 --- 务必谨慎 */
extern FILE *AllocateFile(const char *name, const char *mode);
extern int	FreeFile(FILE *file);

/* 允许使用管道流 (popen/pclose) 的操作 */
extern FILE *OpenPipeStream(const char *command, const char *mode);
extern int	ClosePipeStream(FILE *file);

/* 允许使用 <dirent.h> 库例程的操作 */
extern DIR *AllocateDir(const char *dirname);
extern struct dirent *ReadDir(DIR *dir, const char *dirname);
extern struct dirent *ReadDirExtended(DIR *dir, const char *dirname,
									  int elevel);
extern int	FreeDir(DIR *dir);

/* 允许使用普通内核 FD，并自动清理的操作 */
extern int	OpenTransientFile(const char *fileName, int fileFlags);
extern int	OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern int	CloseTransientFile(int fd);

/* 若确实必须用普通内核 FD，用这些 */
extern int	BasicOpenFile(const char *fileName, int fileFlags);
extern int	BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);

/* 其他情形以及长寿命 BasicOpenFile FD 用这些 */
extern bool AcquireExternalFD(void);
extern void ReserveExternalFD(void);
extern void ReleaseExternalFD(void);

/* 以默认权限建目录 */
extern int	MakePGDirectory(const char *directoryName);

/* 杂项支持例程 */
extern void InitFileAccess(void);
extern void InitTemporaryFileAccess(void);
extern void set_max_safe_fds(void);
extern void closeAllVfds(void);
extern void SetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern bool TempTablespacesAreSet(void);
extern int	GetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern Oid	GetNextTempTableSpace(void);
extern void AtEOXact_Files(bool isCommit);
extern void AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid);
extern void RemovePgTempFiles(void);
extern void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
								   bool unlink_all);
extern bool looks_like_temp_rel_name(const char *name);

extern int	pg_fsync(int fd);
extern int	pg_fsync_no_writethrough(int fd);
extern int	pg_fsync_writethrough(int fd);
extern int	pg_fdatasync(int fd);
extern void pg_flush_data(int fd, off_t offset, off_t nbytes);
extern int	pg_truncate(const char *path, off_t length);
extern void fsync_fname(const char *fname, bool isdir);
extern int	fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel);
extern int	durable_rename(const char *oldfile, const char *newfile, int elevel);
extern int	durable_unlink(const char *fname, int elevel);
extern void SyncDataDirectory(void);
extern int	data_sync_elevel(int elevel);

/* 文件名成分 */
#define PG_TEMP_FILES_DIR "pgsql_tmp"
#define PG_TEMP_FILE_PREFIX "pgsql_tmp"

#endif							/* FD_H */
