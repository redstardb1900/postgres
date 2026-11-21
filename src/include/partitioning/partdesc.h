/*-------------------------------------------------------------------------
 *
 * partdesc.h
 *
 * Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * src/include/partitioning/partdesc.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARTDESC_H
#define PARTDESC_H

#include "partitioning/partdefs.h"
#include "utils/relcache.h"

/*
 * 分区表各分区的信息。
 *
 * 存在已分离分区时，只缓存含全部（含已分离）
 * 分区的描述符；请求不含已分离分区的描述符时
 * 每次新建。原因是对各调用者可见的已分离分区
 * 集合取决于其快照，很难在正确时机从缓存
 * 驱逐描述符。
 * 。
 * 。
 */
typedef struct PartitionDescData
{
	int			nparts;			/*  分区个数  */
	bool		detached_exist; /*  是否有已分离分区？  */
	Oid		   *oids;			/*  nparts 元数组，按边界
								 * 顺序存放分区 OIDs */
	bool	   *is_leaf;		/*  nparts 元数组，表示对应
								 * 'oids' 元素是否属于叶分区
								 * 。 */
	PartitionBoundInfo boundinfo;	/*  分区边界集合  */

	/*  缓存 get_partition_for_tuple() 查找  */

	/*
	 * 上次找到的分区在 PartitionBoundInfo 的 datum
	 * 下标，无则 -1。
	 */
	int			last_found_datum_index;

	/*
	 * 上次找到的分区下标，尚未找到则为
	 * -1。
	 */
	int			last_found_part_index;

	/*
	 * 对 LIST 分区，连续多少次待查 datum 命中
	 * boundinfo->datums 中 last_found_datum_index
	 * 处的 datum。对 RANGE 分区，连续多少次待查
	 * datum 落入 last_found_datum_index 对应
	 * boundinfo->datums 项所表分区的范围。
	 * 。
	 * 。
	 */
	int			last_found_count;
} PartitionDescData;


extern PartitionDesc RelationGetPartitionDesc(Relation rel, bool omit_detached);

extern PartitionDirectory CreatePartitionDirectory(MemoryContext mcxt, bool omit_detached);
extern PartitionDesc PartitionDirectoryLookup(PartitionDirectory, Relation);
extern void DestroyPartitionDirectory(PartitionDirectory pdir);

extern Oid	get_default_oid_from_partdesc(PartitionDesc partdesc);

#endif							/* PARTCACHE_H */
