/*-------------------------------------------------------------------------
 *
 * amapi.h
 *	  Postgres 索引访问方法的 API。
 *
 * Copyright (c) 2015-2023, PostgreSQL Global Development Group
 *
 * src/include/access/amapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AMAPI_H
#define AMAPI_H

#include "access/genam.h"

/*
 * 我们不想在此包含规划器头文件，因为大多数索引
 * 访问方法的实现并不关心这些数据结构。为了允许
 * 在此声明 amcostestimate_function，使用 struct 前向引用。
 */
struct PlannerInfo;
struct IndexPath;

/* 同样，本文件不应依赖 execnodes.h。 */
struct IndexInfo;


/*
 * amproperty API 的属性。此列表涵盖核心代码已知的
 * 属性，但索引访问方法可定义自有属性，其方式为匹配
 * 属性名字符串。
 */
typedef enum IndexAMProperty
{
	AMPROP_UNKNOWN = 0,			/* 任何核心代码未知属性 */
	AMPROP_ASC,					/* 列属性 */
	AMPROP_DESC,
	AMPROP_NULLS_FIRST,
	AMPROP_NULLS_LAST,
	AMPROP_ORDERABLE,
	AMPROP_DISTANCE_ORDERABLE,
	AMPROP_RETURNABLE,
	AMPROP_SEARCH_ARRAY,
	AMPROP_SEARCH_NULLS,
	AMPROP_CLUSTERABLE,			/* 索引属性 */
	AMPROP_INDEX_SCAN,
	AMPROP_BITMAP_SCAN,
	AMPROP_BACKWARD_SCAN,
	AMPROP_CAN_ORDER,			/* AM 属性 */
	AMPROP_CAN_UNIQUE,
	AMPROP_CAN_MULTI_COL,
	AMPROP_CAN_EXCLUDE,
	AMPROP_CAN_INCLUDE
} IndexAMProperty;

/*
 * 我们用此结构体类型的列表，同时跟踪操作符和
 * 支持函数（构建或向操作符类或操作符族添加时）。
 * amadjustmembers 函数接收这些结构体的列表，并允许
 * 修改其 "ref" 字段。
 *
 * "ref" 字段定义 pg_amop 或 pg_amproc 条目应如何依赖
 * 相关联的对象（即使用哪种依赖类型，以及该条目
 * 应依赖的操作符类或操作符族）。
 *
 * 若 ref_is_hard 为 true，则该条目对操作符或支持函数
 * 有 NORMAL 依赖，并对操作符类或操作符族有 INTERNAL
 * 依赖。这会强制在操作符或支持函数被删除时
 * 连带删除操作符类或操作符族，且需 CASCADE 选项
 * 才能删除。也不允许 ALTER OPERATOR FAMILY DROP。
 * 这是对操作符类至关重要的对象的正确行为。
 *
 * 若 ref_is_hard 为 false，则该条目对操作符或支持函数
 * 有 AUTO 依赖，并对操作符类或操作符族也有 AUTO
 * 依赖。这允许 ALTER OPERATOR FAMILY DROP，并使其在
 * 操作符或支持函数被删除时自动发生。这
 * 是对非必需（"松散"）对象的正确行为。
 *
 * 我们还对 lefttype/righttype 建立依赖，强度与
 * 对操作符或支持函数的依赖相同，除非这些依赖
 * 与对操作符或支持函数的依赖冗余。
 */
typedef struct OpFamilyMember
{
	bool		is_func;		/* 是操作符还是支持函数？ */
	Oid			object;			/* 操作符或支持函数 OID */
	int			number;			/* 策略或支持函数编号 */
	Oid			lefttype;		/* lefttype */
	Oid			righttype;		/* righttype */
	Oid			sortfamily;		/* 排序操作符的排序 opfamily 或 0 */
	bool		ref_is_hard;	/* 硬还是软依赖？ */
	bool		ref_is_family;	/* 依赖于 opclass 还是 opfamily？ */
	Oid			refobjid;		/* opclass/opfamily 的 OID */
} OpFamilyMember;


/*
 * 回调函数签名 --- 更多信息见 indexam.sgml。
 */

/* 构建新索引 */
typedef IndexBuildResult *(*ambuild_function) (Relation heapRelation,
											   Relation indexRelation,
											   struct IndexInfo *indexInfo);

/* 构建空索引 */
typedef void (*ambuildempty_function) (Relation indexRelation);

/* 插入此元组 */
typedef bool (*aminsert_function) (Relation indexRelation,
								   Datum *values,
								   bool *isnull,
								   ItemPointer heap_tid,
								   Relation heapRelation,
								   IndexUniqueCheck checkUnique,
								   bool indexUnchanged,
								   struct IndexInfo *indexInfo);

/* 批删除 */
typedef IndexBulkDeleteResult *(*ambulkdelete_function) (IndexVacuumInfo *info,
														 IndexBulkDeleteResult *stats,
														 IndexBulkDeleteCallback callback,
														 void *callback_state);

/* VACUUM 后清理 */
typedef IndexBulkDeleteResult *(*amvacuumcleanup_function) (IndexVacuumInfo *info,
															IndexBulkDeleteResult *stats);

/* 索引扫描可返 IndexTuples？ */
typedef bool (*amcanreturn_function) (Relation indexRelation, int attno);

/* 估算索引扫描的代价 */
typedef void (*amcostestimate_function) (struct PlannerInfo *root,
										 struct IndexPath *path,
										 double loop_count,
										 Cost *indexStartupCost,
										 Cost *indexTotalCost,
										 Selectivity *indexSelectivity,
										 double *indexCorrelation,
										 double *indexPages);

/* 解析索引选项 */
typedef bytea *(*amoptions_function) (Datum reloptions,
									  bool validate);

/* 报告 AM、索引或索引列的属性 */
typedef bool (*amproperty_function) (Oid index_oid, int attno,
									 IndexAMProperty prop, const char *propname,
									 bool *res, bool *isnull);

/* 进度报告中使用的阶段名称 */
typedef char *(*ambuildphasename_function) (int64 phasenum);

/* 校验此 AM 的操作符类定义 */
typedef bool (*amvalidate_function) (Oid opclassoid);

/* 校验要添加到操作符类/操作符族的操作符和支持函数 */
typedef void (*amadjustmembers_function) (Oid opfamilyoid,
										  Oid opclassoid,
										  List *operators,
										  List *functions);

/* 准备索引扫描 */
typedef IndexScanDesc (*ambeginscan_function) (Relation indexRelation,
											   int nkeys,
											   int norderbys);

/* (重)启索引扫描 */
typedef void (*amrescan_function) (IndexScanDesc scan,
								   ScanKey keys,
								   int nkeys,
								   ScanKey orderbys,
								   int norderbys);

/* 下有效元组 */
typedef bool (*amgettuple_function) (IndexScanDesc scan,
									 ScanDirection direction);

/* 取全部有效元组 */
typedef int64 (*amgetbitmap_function) (IndexScanDesc scan,
									   TIDBitmap *tbm);

/* 结束扫描 */
typedef void (*amendscan_function) (IndexScanDesc scan);

/* 标记当前扫描位置 */
typedef void (*ammarkpos_function) (IndexScanDesc scan);

/* 恢复标记的扫描位置 */
typedef void (*amrestrpos_function) (IndexScanDesc scan);

/*
 * 回调函数签名 - 用于并行索引扫描。
 */

/* 估算并行扫描描述符的大小 */
typedef Size (*amestimateparallelscan_function) (void);

/* 准备并行索引扫描 */
typedef void (*aminitparallelscan_function) (void *target);

/* (重)启动并行索引扫描 */
typedef void (*amparallelrescan_function) (IndexScanDesc scan);

/*
 * 索引访问方法 API 结构体。注意它必须存放在单个 palloc
 * 内存块中。
 */
typedef struct IndexAmRoutine
{
	NodeTag		type;

	/*
	 * 策略（操作符）总数，我们可借此遍历/搜索
	 * 此访问方法。若访问方法无固定策略分配集则为零。
	 */
	uint16		amstrategies;
	/* 此 AM 使用的支持函数总数 */
	uint16		amsupport;
	/* 操作符类选项支持函数编号，或 0 */
	uint16		amoptsprocnum;
	/* AM 是否支持按索引列的值 ORDER BY？ */
	bool		amcanorder;
	/* AM 是否支持按索引列上操作符的结果 ORDER BY？ */
	bool		amcanorderbyop;
	/* AM 是否支持反向扫描？ */
	bool		amcanbackward;
	/* AM 是否支持唯一索引？ */
	bool		amcanunique;
	/* AM 是否支持多列索引？ */
	bool		amcanmulticol;
	/* AM 是否要求扫描对第一个索引列有约束？ */
	bool		amoptionalkey;
	/* AM 处理 ScalarArrayOpExpr 条件？ */
	bool		amsearcharray;
	/* AM 处理 IS NULL/IS NOT NULL 条件？ */
	bool		amsearchnulls;
	/* 索引存储数据类型能否不同于列数据类型？ */
	bool		amstorage;
	/* 此类索引能否用于聚簇？ */
	bool		amclusterable;
	/* AM 是否处理谓词锁？ */
	bool		ampredlocks;
	/* AM 是否支持并行扫描？ */
	bool		amcanparallel;
	/* AM 是否支持 INCLUDE 子句包含的列？ */
	bool		amcaninclude;
	/* AM 使用 maintenance_work_mem？ */
	bool		amusemaintenanceworkmem;
	/* AM 是否仅按块粒度存储元组信息？ */
	bool		amsummarizing;
	/* 并行 vacuum 标志的 OR。标志见 vacuum.h。 */
	uint8		amparallelvacuumoptions;
	/* 索引存储的数据类型，可变则为 InvalidOid */
	Oid			amkeytype;

	/*
	 * 若向上述或下述任一列表新增属性，则它们（通常）
	 * 也应通过属性 API 公开（见
	 * 文件开头的 IndexAMProperty 以及 utils/adt/amutils.c）。
	 */

	/* 接口函数 */
	ambuild_function ambuild;
	ambuildempty_function ambuildempty;
	aminsert_function aminsert;
	ambulkdelete_function ambulkdelete;
	amvacuumcleanup_function amvacuumcleanup;
	amcanreturn_function amcanreturn;	/* 可为 NULL */
	amcostestimate_function amcostestimate;
	amoptions_function amoptions;
	amproperty_function amproperty; /* 可为 NULL */
	ambuildphasename_function ambuildphasename; /* 可为 NULL */
	amvalidate_function amvalidate;
	amadjustmembers_function amadjustmembers;	/* 可为 NULL */
	ambeginscan_function ambeginscan;
	amrescan_function amrescan;
	amgettuple_function amgettuple; /* 可为 NULL */
	amgetbitmap_function amgetbitmap;	/* 可为 NULL */
	amendscan_function amendscan;
	ammarkpos_function ammarkpos;	/* 可为 NULL */
	amrestrpos_function amrestrpos; /* 可为 NULL */

	/* 支持并行索引扫描的接口函数 */
	amestimateparallelscan_function amestimateparallelscan; /* 可为 NULL */
	aminitparallelscan_function aminitparallelscan; /* 可为 NULL */
	amparallelrescan_function amparallelrescan; /* 可为 NULL */
} IndexAmRoutine;


/* access/index/amapi.c 中的函数 */
extern IndexAmRoutine *GetIndexAmRoutine(Oid amhandler);
extern IndexAmRoutine *GetIndexAmRoutineByAmId(Oid amoid, bool noerror);

#endif							/* AMAPI_H */
