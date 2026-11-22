/*-------------------------------------------------------------------------
 *
 * old_snapshot.h
 *			"snapshot too old" 的数据结构
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/old_snapshot.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OLD_SNAPSHOT_H
#define OLD_SNAPSHOT_H

#include "datatype/timestamp.h"
#include "storage/s_lock.h"

/*
 * 处理 old_snapshot_threshold 实现的结构
 */
typedef struct OldSnapshotControlData
{
	/*
	 * 旧快照处理变量在进程间共享，阈值
	 * 只允许向前推进。
	 */
	slock_t		mutex_current;	/* 保护 current_timestamp */
	TimestampTz current_timestamp;	/* 最新快照时间戳 */
	slock_t		mutex_latest_xmin;	/* 保护 latest_xmin 与 next_map_update */
	TransactionId latest_xmin;	/* 最新快照 xmin */
	TimestampTz next_map_update;	/* 最新快照有效至 */
	slock_t		mutex_threshold;	/* 保护阈值字段 */
	TimestampTz threshold_timestamp;	/* 更早快照已过旧 */
	TransactionId threshold_xid;	/* 更早 xid 或已消失 */

	/*
	 * Keep one xid per minute for old snapshot error handling.
	 *
	 * Use a circular buffer with a head offset, a count of entries currently
	 * used, and a timestamp corresponding to the xid at the head offset.  A
	 * count_used value of zero means that there are no times stored; a
	 * count_used value of OLD_SNAPSHOT_TIME_MAP_ENTRIES means that the buffer
	 * is full and the head must be advanced to add new entries.  Use
	 * timestamps aligned to minute boundaries, since that seems less
	 * surprising than aligning based on the first usage timestamp.  The
	 * latest bucket is effectively stored within latest_xmin.  The circular
	 * buffer is updated when we get a new xmin value that doesn't fall into
	 * the same interval.
	 *
	 * It is OK if the xid for a given time slot is from earlier than
	 * calculated by adding the number of minutes corresponding to the
	 * (possibly wrapped) distance from the head offset to the time of the
	 * head entry, since that just results in the vacuuming of old tuples
	 * being slightly less aggressive.  It would not be OK for it to be off in
	 * the other direction, since it might result in vacuuming tuples that are
	 * still expected to be there.
	 *
	 * Use of an SLRU was considered but not chosen because it is more
	 * heavyweight than is needed for this, and would probably not be any less
	 * code to implement.
	 *
	 * Persistence is not needed.
	 */
	int			head_offset;	/* 最旧跟踪时间的下标 */
	TimestampTz head_timestamp; /* 对应头 xid 的时间 */
	int			count_used;		/* 已用槽数 */
	TransactionId xid_by_minute[FLEXIBLE_ARRAY_MEMBER];
} OldSnapshotControlData;

extern PGDLLIMPORT volatile OldSnapshotControlData *oldSnapshotControl;

#endif
