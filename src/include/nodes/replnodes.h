/*-------------------------------------------------------------------------
 *
 * replnodes.h
 *	复制语法解析节点的定义
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/replnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLNODES_H
#define REPLNODES_H

#include "access/xlogdefs.h"
#include "nodes/pg_list.h"

typedef enum ReplicationKind
{
	REPLICATION_KIND_PHYSICAL,
	REPLICATION_KIND_LOGICAL
} ReplicationKind;


/* ----------------------
 *	IDENTIFY_SYSTEM 命令
 * ----------------------
 */
typedef struct IdentifySystemCmd
{
	NodeTag		type;
} IdentifySystemCmd;


/* ----------------------
 *	BASE_BACKUP 命令
 * ----------------------
 */
typedef struct BaseBackupCmd
{
	NodeTag		type;
	List	   *options;
} BaseBackupCmd;


/* ----------------------
 *	CREATE_REPLICATION_SLOT 命令
 * ----------------------
 */
typedef struct CreateReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	ReplicationKind kind;
	char	   *plugin;
	bool		temporary;
	List	   *options;
} CreateReplicationSlotCmd;


/* ----------------------
 *	DROP_REPLICATION_SLOT 命令
 * ----------------------
 */
typedef struct DropReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
	bool		wait;
} DropReplicationSlotCmd;


/* ----------------------
 *	START_REPLICATION 命令
 * ----------------------
 */
typedef struct StartReplicationCmd
{
	NodeTag		type;
	ReplicationKind kind;
	char	   *slotname;
	TimeLineID	timeline;
	XLogRecPtr	startpoint;
	List	   *options;
} StartReplicationCmd;


/* ----------------------
 *	READ_REPLICATION_SLOT 命令
 * ----------------------
 */
typedef struct ReadReplicationSlotCmd
{
	NodeTag		type;
	char	   *slotname;
} ReadReplicationSlotCmd;


/* ----------------------
 *	TIMELINE_HISTORY 命令
 * ----------------------
 */
typedef struct TimeLineHistoryCmd
{
	NodeTag		type;
	TimeLineID	timeline;
} TimeLineHistoryCmd;

#endif							/* REPLNODES_H */
