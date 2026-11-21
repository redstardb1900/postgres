/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  PostgreSQL 大对象声明。POSTGRES 4.2 曾支持
 *	  海量大对象（internal、external、jaquith、inversion）。
 *	  现仅支持 inversion。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/large_object.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LARGE_OBJECT_H
#define LARGE_OBJECT_H

#include "utils/snapshot.h"


/*----------
 * 当前已打开大对象的数据。
 *
 * id 是大对象的逻辑 OID
 * snapshot 是读/写操作所用的快照
 * subid 是打开该描述符的子事务（或当前拥有者）
 * offset 是 LO 内当前 seek 偏移
 * flags 含若干标志位
 *
 * 注意：自 v11 起，权限检查在打开大对象时进行；
 * 故 IFS_RDLOCK/IFS_WRLOCK 表示请求了读或写模式
 * *且*已检查对应权限。
 *
 * 注意：7.1 以前还须保存指向各大对象独立表与
 * 索引的引用。现均存于 pg_largeobject，经共用
 * 关系描述符访问。
 *----------
 */
typedef struct LargeObjectDesc
{
	Oid			id;				/* LO 标识符 */
	Snapshot	snapshot;		/* 所用快照 */
	SubTransactionId subid;		/* 拥有子事务 ID */
	uint64		offset;			/* 当前 seek 指针 */
	int			flags;			/* 见下方标志位 */

/* flags 位： */
#define IFS_RDLOCK		(1 << 0)	/* LO 已为读而打开 */
#define IFS_WRLOCK		(1 << 1)	/* LO 已为写而打开 */

} LargeObjectDesc;


/*
 * 大对象每个"页"（元组）可容纳这么多数据
 *
 * 本可设为 BLCKSZ 减去若干开销，但似乎更宜
 * 取较小值，以免更新页元组时占用过多空间。
 * 注意：该值有意取大到足以触发元组 toaster，
 * 以便我们尝试就地压缩页元组。（但除非
 * 用户为 pg_largeobject 建 toast 表，否则不会
 * 被移出页外……）
 *
 * 另，把页大小定为 2 的幂也很明智，因客户端常按 
 * 2 的幂块发送数据。这可避免因部分页写入
 * 引起的不必要元组更新。
 *
 * 注意：改 LOBLKSIZE 须 initdb。
 */
#define LOBLKSIZE		(BLCKSZ / 4)

/*
 * 大对象的最大字节长度。要加大须同时加宽 
 * pg_largeobject.pageno 以及若干内部变量。
 */
#define MAX_LARGE_OBJECT_SIZE	((int64) INT_MAX * LOBLKSIZE)


/*
 * GUC：抑制大对象权限检查的向后兼容标志
 */
extern PGDLLIMPORT bool lo_compat_privileges;

/*
 * 函数定义……
 */

/* inversion stuff in inv_api.c */
extern void close_lo_relation(bool isCommit);
extern Oid	inv_create(Oid lobjId);
extern LargeObjectDesc *inv_open(Oid lobjId, int flags, MemoryContext mcxt);
extern void inv_close(LargeObjectDesc *obj_desc);
extern int	inv_drop(Oid lobjId);
extern int64 inv_seek(LargeObjectDesc *obj_desc, int64 offset, int whence);
extern int64 inv_tell(LargeObjectDesc *obj_desc);
extern int	inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes);
extern int	inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes);
extern void inv_truncate(LargeObjectDesc *obj_desc, int64 len);

#endif							/* LARGE_OBJECT_H */
