/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES 元组描述符定义。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct AttrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* 表达式的 nodeToString 表示 */
} AttrDefault;

typedef struct ConstrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* 表达式的 nodeToString 表示 */
	bool		ccvalid;
	bool		ccnoinherit;	/* 这是一个非继承约束 */
} ConstrCheck;

/* 本结构体包含元组的约束 */
typedef struct TupleConstr
{
	AttrDefault *defval;		/* array */
	ConstrCheck *check;			/* array */
	struct AttrMissing *missing;	/* 缺失属性值，无则 NULL */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;
	bool		has_generated_stored;
} TupleConstr;

/*
 * 本结构体在后端内传递，用以描述
 * 元组的结构。对来自磁盘关系的元组，其信息
 * 采自 pg_attribute、pg_attrdef 与 pg_constraint 目录。
 * 瞬时行类型（如连接查询的结果）具有匿名的
 * TupleDesc 结构体，通常省略任何约束信息；因此该
 * 结构体设计为可高效地省略约束。
 *
 * 注意：其中仅提及用户属性，不提及系统属性，见
 * TupleDesc.
 *
 * 若已知 tupdesc 对应命名行类型（如表的
 * 行类型），则 tdtypeid 标识该类型且 tdtypmod 为 -1。否则
 * tdtypeid 为 RECORDOID，而 tdtypmod 可为 -1 表示完全匿名
 * 行类型，或为 >= 0 的值以便在 typcache.c 类型缓存
 * 中查找行类型。
 *
 * 注意：tdtypeid 从不是复合上域的 OID，即便
 * 我们处理的值在更高层已知属于
 * 复合类型上的域。这是因为 tdtypeid/tdtypmod 须与
 * 复合 Datum 的类型标注一致，而那些也从不
 * 被显式标为域类型。
 *
 * 缓存中的元组描述符（目前为 relcache 或 typcache）
 * 做引用计数：当其引用计数降到
 * 零时可删除。执行器创建的元组描述符则无需引用
 * 计数：它们只是在适当的内存
 * 上下文中创建，上下文释放时即消失。我们把此类
 * 描述符的 tdrefcount 设为 -1，做引用计数的则
 * 始终有 tdrefcount >= 0。
 */
typedef struct TupleDescData
{
	int			natts;			/* 元组中的属性个数 */
	Oid			tdtypeid;		/* 元组类型的复合类型 ID */
	int32		tdtypmod;		/* 元组类型 typmod */
	int			tdrefcount;		/* 引用计数，不计则为 -1 */
	TupleConstr *constr;		/* 约束，无则 NULL */
	/* attrs[N] 是属性号 N+1 的描述 */
	FormData_pg_attribute attrs[FLEXIBLE_ARRAY_MEMBER];
}			TupleDescData;
typedef struct TupleDescData *TupleDesc;

/* tupdesc 第 i 个属性的访问器。 */
#define TupleDescAttr(tupdesc, i) (&(tupdesc)->attrs[(i)])

extern TupleDesc CreateTemplateTupleDesc(int natts);

extern TupleDesc CreateTupleDesc(int natts, Form_pg_attribute *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

#define TupleDescSize(src) \
	(offsetof(struct TupleDescData, attrs) + \
	 (src)->natts * sizeof(FormData_pg_attribute))

extern void TupleDescCopy(TupleDesc dst, TupleDesc src);

extern void TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
							   TupleDesc src, AttrNumber srcAttno);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern void IncrTupleDescRefCount(TupleDesc tupdesc);
extern void DecrTupleDescRefCount(TupleDesc tupdesc);

#define PinTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			IncrTupleDescRefCount(tupdesc); \
	} while (0)

#define ReleaseTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			DecrTupleDescRefCount(tupdesc); \
	} while (0)

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);

extern uint32 hashTupleDesc(TupleDesc desc);

extern void TupleDescInitEntry(TupleDesc desc,
							   AttrNumber attributeNumber,
							   const char *attributeName,
							   Oid oidtypeid,
							   int32 typmod,
							   int attdim);

extern void TupleDescInitBuiltinEntry(TupleDesc desc,
									  AttrNumber attributeNumber,
									  const char *attributeName,
									  Oid oidtypeid,
									  int32 typmod,
									  int attdim);

extern void TupleDescInitEntryCollation(TupleDesc desc,
										AttrNumber attributeNumber,
										Oid collationid);

extern TupleDesc BuildDescForRelation(List *schema);

extern TupleDesc BuildDescFromLists(List *names, List *types, List *typmods, List *collations);

#endif							/* TUPDESC_H */
