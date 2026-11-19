/*-------------------------------------------------------------------------
 *
 * spgist.h
 *	  SP-GiST 访问方法的公开头文件。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/spgist.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SPGIST_H
#define SPGIST_H

#include "access/amapi.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"


/* SPGiST 操作符类支持函数编号 */
#define SPGIST_CONFIG_PROC				1
#define SPGIST_CHOOSE_PROC				2
#define SPGIST_PICKSPLIT_PROC			3
#define SPGIST_INNER_CONSISTENT_PROC	4
#define SPGIST_LEAF_CONSISTENT_PROC		5
#define SPGIST_COMPRESS_PROC			6
#define SPGIST_OPTIONS_PROC				7
#define SPGISTNRequiredProc				5
#define SPGISTNProc						7

/*
 * spg_config 方法的参数结构
 */
typedef struct spgConfigIn
{
	Oid			attType;		/* 待索引数据类型 */
} spgConfigIn;

typedef struct spgConfigOut
{
	Oid			prefixType;		/* 内部元组前缀的数据类型 */
	Oid			labelType;		/* 内部元组节点标签数据类型 */
	Oid			leafType;		/* 叶子元组值的数据类型 */
	bool		canReturnData;	/* 操作符类可重建原始数据 */
	bool		longValuesOK;	/* 操作符类可处理 > 1 页之值 */
} spgConfigOut;

/*
 * spg_choose 方法的参数结构
 */
typedef struct spgChooseIn
{
	Datum		datum;			/* 待索引的原始 datum */
	Datum		leafDatum;		/* 当前将存于叶子的 datum */
	int			level;			/* 当前层级（从零计） */

	/* 当前内部元组的数据 */
	bool		allTheSame;		/* 元组标为 all-the-same？ */
	bool		hasPrefix;		/* 元组有前缀？ */
	Datum		prefixDatum;	/* 若有，则前缀值 */
	int			nNodes;			/* 内部元组中的节点数 */
	Datum	   *nodeLabels;		/* 节点标签值(无则为 NULL) */
} spgChooseIn;

typedef enum spgChooseResultType
{
	spgMatchNode = 1,			/* 下降到已有节点 */
	spgAddNode,					/* 向内部元组添加节点 */
	spgSplitTuple				/* 分裂内部元组（改其前缀） */
} spgChooseResultType;

typedef struct spgChooseOut
{
	spgChooseResultType resultType; /* 动作码，见上文 */
	union
	{
		struct					/* spgMatchNode 的结果 */
		{
			int			nodeN;	/* 下降到此节点（下标从 0） */
			int			levelAdd;	/* 层级增加这么多 */
			Datum		restDatum;	/* 新叶 datum */
		}			matchNode;
		struct					/* spgAddNode 的结果 */
		{
			Datum		nodeLabel;	/* 新节点标签 */
			int			nodeN;	/* 插入位置（下标从 0） */
		}			addNode;
		struct					/* spgSplitTuple 的结果 */
		{
			/* 构造含一个子元组的新上层内部元组的信息 */
			bool		prefixHasPrefix;	/* 元组应有前缀？ */
			Datum		prefixPrefixDatum;	/* 若有则其值 */
			int			prefixNNodes;	/* 节点数 */
			Datum	   *prefixNodeLabels;	/* 其标签(或 NULL 表示无
											 * 标签)*/
			int			childNodeN; /* 哪个节点获得子元组 */

			/* 构造含全部旧节点的新下层内部元组的信息 */
			bool		postfixHasPrefix;	/* 元组应有前缀？ */
			Datum		postfixPrefixDatum; /* 若有则其值 */
		}			splitTuple;
	}			result;
} spgChooseOut;

/*
 * spg_picksplit 方法的参数结构
 */
typedef struct spgPickSplitIn
{
	int			nTuples;		/* 叶子元组数目 */
	Datum	   *datums;			/* 其 datums(长度 nTuples 的数组) */
	int			level;			/* 当前层级（从零计） */
} spgPickSplitIn;

typedef struct spgPickSplitOut
{
	bool		hasPrefix;		/* 新内部元组应有前缀？ */
	Datum		prefixDatum;	/* 若有则其值 */

	int			nNodes;			/* 新内部元组的节点数 */
	Datum	   *nodeLabels;		/* 其标签(或 NULL 表示无标签) */

	int		   *mapTuplesToNodes;	/* 各叶子元组节点下标 */
	Datum	   *leafTupleDatums;	/* 存入每个新叶子元组的 datum */
} spgPickSplitOut;

/*
 * spg_inner_consistent 方法的参数结构
 */
typedef struct spgInnerConsistentIn
{
	ScanKey		scankeys;		/* 操作符与比较值的数组 */
	ScanKey		orderbys;		/* 排序操作符与比较值的
								 * 数组*/
	int			nkeys;			/* scankeys 数组长度 */
	int			norderbys;		/* orderbys 数组长度 */

	Datum		reconstructedValue; /* 在父级重建的值 */
	void	   *traversalValue; /* 操作符类专用遍历值 */
	MemoryContext traversalMemoryContext;	/* 新遍历值放此处 */
	int			level;			/* 当前层级（从零计） */
	bool		returnData;		/* 必须返回原始数据？ */

	/* 当前内部元组的数据 */
	bool		allTheSame;		/* 元组标为 all-the-same？ */
	bool		hasPrefix;		/* 元组有前缀？ */
	Datum		prefixDatum;	/* 若有，则前缀值 */
	int			nNodes;			/* 内部元组中的节点数 */
	Datum	   *nodeLabels;		/* 节点标签值(无则为 NULL) */
} spgInnerConsistentIn;

typedef struct spgInnerConsistentOut
{
	int			nNodes;			/* 待访问的子节点数 */
	int		   *nodeNumbers;	/* 其在节点数组中的下标 */
	int		   *levelAdds;		/* 各自层级增加这么多 */
	Datum	   *reconstructedValues;	/* 关联的重建值 */
	void	  **traversalValues;	/* 操作符类专用遍历值 */
	double	  **distances;		/* 关联的距离 */
} spgInnerConsistentOut;

/*
 * spg_leaf_consistent 方法的参数结构
 */
typedef struct spgLeafConsistentIn
{
	ScanKey		scankeys;		/* 操作符与比较值的数组 */
	ScanKey		orderbys;		/* 排序操作符与比较值的
								 * 数组*/
	int			nkeys;			/* scankeys 数组长度 */
	int			norderbys;		/* orderbys 数组长度 */

	Datum		reconstructedValue; /* 在父级重建的值 */
	void	   *traversalValue; /* 操作符类专用遍历值 */
	int			level;			/* 当前层级（从零计） */
	bool		returnData;		/* 必须返回原始数据？ */

	Datum		leafDatum;		/* 叶元组内 datum */
} spgLeafConsistentIn;

typedef struct spgLeafConsistentOut
{
	Datum		leafValue;		/* 重建的原始数据（若有） */
	bool		recheck;		/* 若操作符须重检则为 T */
	bool		recheckDistances;	/* 若距离须重检则为 T */
	double	   *distances;		/* 关联的距离 */
} spgLeafConsistentOut;


/* spgutils.c */
extern bytea *spgoptions(Datum reloptions, bool validate);

/* spginsert.c */
extern IndexBuildResult *spgbuild(Relation heap, Relation index,
								  struct IndexInfo *indexInfo);
extern void spgbuildempty(Relation index);
extern bool spginsert(Relation index, Datum *values, bool *isnull,
					  ItemPointer ht_ctid, Relation heapRel,
					  IndexUniqueCheck checkUnique,
					  bool indexUnchanged,
					  struct IndexInfo *indexInfo);

/* spgscan.c */
extern IndexScanDesc spgbeginscan(Relation rel, int keysz, int orderbysz);
extern void spgendscan(IndexScanDesc scan);
extern void spgrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					  ScanKey orderbys, int norderbys);
extern int64 spggetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern bool spggettuple(IndexScanDesc scan, ScanDirection dir);
extern bool spgcanreturn(Relation index, int attno);

/* spgvacuum.c */
extern IndexBulkDeleteResult *spgbulkdelete(IndexVacuumInfo *info,
											IndexBulkDeleteResult *stats,
											IndexBulkDeleteCallback callback,
											void *callback_state);
extern IndexBulkDeleteResult *spgvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);

/* spgvalidate.c */
extern bool spgvalidate(Oid opclassoid);
extern void spgadjustmembers(Oid opfamilyoid,
							 Oid opclassoid,
							 List *operators,
							 List *functions);

#endif							/* SPGIST_H */
