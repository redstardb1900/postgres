/*-------------------------------------------------------------------------
 *
 * pg_control.h
 *	  The system control file "pg_control" is not a heap relation.
 *	  However, we define it here so that the format is documented.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_control.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONTROL_H
#define PG_CONTROL_H

#include "access/transam.h"
#include "access/xlogdefs.h"
#include "pgtime.h"				/* for pg_time_t */
#include "port/pg_crc32c.h"


/*  本 pg_control 格式的版本标识  */
#define PG_CONTROL_VERSION	1300

/*  Nonce 密钥长度,见下  */
#define MOCK_AUTH_NONCE_LEN		32

/*
 * Body of CheckPoint XLOG records.  This is declared here because we keep
 * a copy of the latest one in pg_control for possible disaster recovery.
 * Changing this struct requires a PG_CONTROL_VERSION bump.
 */
typedef struct CheckPoint
{
	XLogRecPtr	redo;			/* next RecPtr available when we began to
								 * create CheckPoint (i.e. REDO start point) */
	TimeLineID	ThisTimeLineID; /* 当前 TLI */
	TimeLineID	PrevTimeLineID; /* previous TLI, if this record begins a new
								 * timeline (equals ThisTimeLineID otherwise) */
	bool		fullPageWrites; /* 当前 full_page_writes */
	FullTransactionId nextXid;	/*  下一可用事务 ID  */
	Oid			nextOid;		/* next free OID */
	MultiXactId nextMulti;		/* next free MultiXactId */
	MultiXactOffset nextMultiOffset;	/* next free MultiXact offset */
	TransactionId oldestXid;	/*  集簇范围最小 datfrozenxid  */
	Oid			oldestXidDB;	/* database with minimum datfrozenxid */
	MultiXactId oldestMulti;	/*  集簇范围最小 datminmxid  */
	Oid			oldestMultiDB;	/* database with minimum datminmxid */
	pg_time_t	time;			/*  检查点时间戳  */
	TransactionId oldestCommitTsXid;	/* 有有效提交的最旧 Xid
										 * 时间戳 */
	TransactionId newestCommitTsXid;	/* 有有效提交的最新 Xid
										 * 时间戳 */

	/*
	 * Oldest XID still running. This is only needed to initialize hot standby
	 * mode from an online checkpoint, so we only bother calculating this for
	 * online checkpoints and only when wal_level is replica. Otherwise it's
	 * set to InvalidTransactionId.
	 */
	TransactionId oldestActiveXid;
} CheckPoint;

/* XLOG info values for XLOG rmgr */
#define XLOG_CHECKPOINT_SHUTDOWN		0x00
#define XLOG_CHECKPOINT_ONLINE			0x10
#define XLOG_NOOP						0x20
#define XLOG_NEXTOID					0x30
#define XLOG_SWITCH						0x40
#define XLOG_BACKUP_END					0x50
#define XLOG_PARAMETER_CHANGE			0x60
#define XLOG_RESTORE_POINT				0x70
#define XLOG_FPW_CHANGE					0x80
#define XLOG_END_OF_RECOVERY			0x90
#define XLOG_FPI_FOR_HINT				0xA0
#define XLOG_FPI						0xB0
/*  Postgres 9.5-11 使用 0xC0  */
#define XLOG_OVERWRITE_CONTRECORD		0xD0


/*
 * System status indicator.  Note this is stored in pg_control; if you change
 * it, you must bump PG_CONTROL_VERSION
 */
typedef enum DBState
{
	DB_STARTUP = 0,
	DB_SHUTDOWNED,
	DB_SHUTDOWNED_IN_RECOVERY,
	DB_SHUTDOWNING,
	DB_IN_CRASH_RECOVERY,
	DB_IN_ARCHIVE_RECOVERY,
	DB_IN_PRODUCTION
} DBState;

/*
 * Contents of pg_control.
 */

typedef struct ControlFileData
{
	/*
	 * Unique system identifier --- to ensure we match up xlog files with the
	 * installation that produced them.
	 */
	uint64		system_identifier;

	/*
	 * Version identifier information.  Keep these fields at the same offset,
	 * especially pg_control_version; they won't be real useful if they move
	 * around.  (For historical reasons they must be 8 bytes into the file
	 * rather than immediately at the front.)
	 *
	 * pg_control_version identifies the format of pg_control itself.
	 * catalog_version_no identifies the format of the system catalogs.
	 *
	 * There are additional version identifiers in individual files; for
	 * example, WAL logs contain per-page magic numbers that can serve as
	 * version cues for the WAL log.
	 */
	uint32		pg_control_version; /* PG_CONTROL_VERSION */
	uint32		catalog_version_no; /* 见 catversion.h */

	/*
	 * 系统状态数据
	 */
	DBState		state;			/* see enum above */
	pg_time_t	time;			/* 上次 pg_control 更新的时间戳 */
	XLogRecPtr	checkPoint;		/* 最近检查点记录指针 */

	CheckPoint	checkPointCopy; /* 最近检查点记录的副本 */

	XLogRecPtr	unloggedLSN;	/*  当前假 LSN,用于不记日志关系  */

	/*
	 * These two values determine the minimum point we must recover up to
	 * before starting up:
	 *
	 * minRecoveryPoint is updated to the latest replayed LSN whenever we
	 * flush a data change during archive recovery. That guards against
	 * starting archive recovery, aborting it, and restarting with an earlier
	 * stop location. If we've already flushed data changes from WAL record X
	 * to disk, we mustn't start up until we reach X again. Zero when not
	 * doing archive recovery.
	 *
	 * backupStartPoint is the redo pointer of the backup start checkpoint, if
	 * we are recovering from an online backup and haven't reached the end of
	 * backup yet. It is reset to zero when the end of backup is reached, and
	 * we mustn't start up before that. A boolean would suffice otherwise, but
	 * we use the redo pointer as a cross-check when we see an end-of-backup
	 * record, to make sure the end-of-backup record corresponds the base
	 * backup we're recovering from.
	 *
	 * backupEndPoint is the backup end location, if we are recovering from an
	 * online backup which was taken from the standby and haven't reached the
	 * end of backup yet. It is initialized to the minimum recovery point in
	 * pg_control which was backed up last. It is reset to zero when the end
	 * of backup is reached, and we mustn't start up before that.
	 *
	 * If backupEndRequired is true, we know for sure that we're restoring
	 * from a backup, and must see a backup-end record before we can safely
	 * start up.
	 */
	XLogRecPtr	minRecoveryPoint;
	TimeLineID	minRecoveryPointTLI;
	XLogRecPtr	backupStartPoint;
	XLogRecPtr	backupEndPoint;
	bool		backupEndRequired;

	/*
	 * Parameter settings that determine if the WAL can be used for archival
	 * or hot standby.
	 */
	int			wal_level;
	bool		wal_log_hints;
	int			MaxConnections;
	int			max_worker_processes;
	int			max_wal_senders;
	int			max_prepared_xacts;
	int			max_locks_per_xact;
	bool		track_commit_timestamp;

	/*
	 * This data is used to check for hardware-architecture compatibility of
	 * the database and the backend executable.  We need not check endianness
	 * explicitly, since the pg_control version will surely look wrong to a
	 * machine of different endianness, but we do need to worry about MAXALIGN
	 * and floating-point format.  (Note: storage layout nominally also
	 * depends on SHORTALIGN and INTALIGN, but in practice these are the same
	 * on all architectures of interest.)
	 *
	 * Testing just one double value is not a very bulletproof test for
	 * floating-point compatibility, but it will catch most cases.
	 */
	uint32		maxAlign;		/*  元组对齐要求  */
	double		floatFormat;	/*  常量 1234567.0  */
#define FLOATFORMAT_VALUE	1234567.0

	/*
	 * This data is used to make sure that configuration of this database is
	 * compatible with the backend executable.
	 */
	uint32		blcksz;			/*  本库数据块大小  */
	uint32		relseg_size;	/*  大关系每段块数  */

	uint32		xlog_blcksz;	/*  WAL 文件内块大小  */
	uint32		xlog_seg_size;	/*  每个 WAL 段大小  */

	uint32		nameDataLen;	/*  目录名字段宽度  */
	uint32		indexMaxKeys;	/*  索引最大列数  */

	uint32		toast_max_chunk_size;	/*  TOAST 表块大小  */
	uint32		loblksize;		/*  pg_largeobject 块大小  */

	bool		float8ByVal;	/*  float8、int8 等是否传值?  */

	/*  数据页是否受校验和保护?无校验和版本则为零  */
	uint32		data_checksum_version;

	/*
	 * Random nonce, used in authentication requests that need to proceed
	 * based on values that are cluster-unique, like a SASL exchange that
	 * failed at an early stage.
	 */
	char		mock_authentication_nonce[MOCK_AUTH_NONCE_LEN];

	/* CRC of all above ... MUST BE LAST! */
	pg_crc32c	crc;
} ControlFileData;

/*
 * Maximum safe value of sizeof(ControlFileData).  For reliability's sake,
 * it's critical that pg_control updates be atomic writes.  That generally
 * means the active data can't be more than one disk sector, which is 512
 * bytes on common hardware.  Be very careful about raising this limit.
 */
#define PG_CONTROL_MAX_SAFE_SIZE	512

/*
 * Physical size of the pg_control file.  Note that this is considerably
 * bigger than the actually used size (ie, sizeof(ControlFileData)).
 * The idea is to keep the physical size constant independent of format
 * changes, so that ReadControlFile will deliver a suitable wrong-version
 * message instead of a read error if it's looking at an incompatible file.
 */
#define PG_CONTROL_FILE_SIZE		8192

/*
 * Ensure that the size of the pg_control data structure is sane.
 */
StaticAssertDecl(sizeof(ControlFileData) <= PG_CONTROL_MAX_SAFE_SIZE,
				 "pg_control is too large for atomic disk writes");
StaticAssertDecl(sizeof(ControlFileData) <= PG_CONTROL_FILE_SIZE,
				 "sizeof(ControlFileData) exceeds PG_CONTROL_FILE_SIZE");

#endif							/* PG_CONTROL_H */
