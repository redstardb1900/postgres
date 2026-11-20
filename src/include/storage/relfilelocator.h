/*-------------------------------------------------------------------------
 *
 * relfilelocator.h
 *	  关系的物理访问信息。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/relfilelocator.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILELOCATOR_H
#define RELFILELOCATOR_H

#include "common/relpath.h"
#include "storage/backendid.h"

/*
 * RelFileLocator 必须提供物理访问关系所需的全部
 * 信息，后端 ID 除外，后者可另行提供。
 * 但注意，一个"物理"关系由文件系统上多个
 * 文件组成：每个分支存成单独文件，且每个分支
 * 还可分成多个段。见 md.c。
 *
 * spcOid 标识关系的表空间，对应于
 * pg_tablespace.oid.
 *
 * dbOid 标识关系所属数据库。对"共享"关系（
 * 集簇内各库共有者）为零。
 * 非零 dbOid 对应于 pg_database.oid。
 *
 * relNumber 标识具体关系。relNumber 对应于
 * pg_class.relfilenode（不是 pg_class.oid，因为有时
 * 须能给关系分配新的物理文件）。
 * 注意 relNumber 仅在某表空间的某数据库内
 * 唯一。
 *
 * 注意：spcOid 当且仅当 dbOid 为零时必须是
 * GLOBALTABLESPACE_OID。共享关系仅放在"global"表空间。
 *
 * 注意：pg_class 中允许 reltablespace == 0 表示关系
 * 存放于其数据库的"默认"表空间（由
 * pg_database.dattablespace 标识）。但 RelFileLocator
 * 结构中不允许这种简写——设置 spcOid 时必须
 * 给出真正的表空间 ID。
 *
 * 注意：pg_class 中 relfilenode 可为零，表示该关系是"
 * 映射"关系，当前真实 filenode 号可从
 * relmapper.c 取得。RelFileLocators 中同样不允许。
 *
 * 注意：多处把 RelFileLocator 用作哈希表键。因此
 * 本结构切勿有任何未用填充字节。只要字段
 * 全是 Oid 类型，这就应是安全的。
 */
typedef struct RelFileLocator
{
	Oid			spcOid;			/* 表空间 */
	Oid			dbOid;			/* database */
	RelFileNumber relNumber;	/* 关系 */
} RelFileLocator;

/*
 * 给 relfilelocator 补上后端 ID，即有定位物理存储
 * 所需全部信息。常规关系（多个后端可访问）的
 * 后端 ID 为 InvalidBackendId；后端局部关系则为
 * 所属后端的 ID。后端局部关系总是短暂的，数据库
 * 崩溃时会被删除；它们从不写 WAL，也不
 * fsync。
 */
typedef struct RelFileLocatorBackend
{
	RelFileLocator locator;
	BackendId	backend;
} RelFileLocatorBackend;

#define RelFileLocatorBackendIsTemp(rlocator) \
	((rlocator).backend != InvalidBackendId)

/*
 * 注意：RelFileLocatorEquals 与 RelFileLocatorBackendEquals 先比较
 * relNumber，因其最可能在两个不等的
 * RelFileLocators 中不同。若其他字段已相等再比
 * spcOid 或许多余，但仍比较以求保险。检查
 * 后端 ID 同理。
 */
#define RelFileLocatorEquals(locator1, locator2) \
	((locator1).relNumber == (locator2).relNumber && \
	 (locator1).dbOid == (locator2).dbOid && \
	 (locator1).spcOid == (locator2).spcOid)

#define RelFileLocatorBackendEquals(locator1, locator2) \
	((locator1).locator.relNumber == (locator2).locator.relNumber && \
	 (locator1).locator.dbOid == (locator2).locator.dbOid && \
	 (locator1).backend == (locator2).backend && \
	 (locator1).locator.spcOid == (locator2).locator.spcOid)

#endif							/* RELFILELOCATOR_H */
