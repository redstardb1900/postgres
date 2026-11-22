/*-------------------------------------------------------------------------
 *
 * expandedrecord.h
 *	  复合扩展对象的声明。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/expandedrecord.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPANDEDRECORD_H
#define EXPANDEDRECORD_H

#include "access/htup.h"
#include "access/tupdesc.h"
#include "fmgr.h"
#include "utils/expandeddatum.h"


/*
 * 扩展记录含于私有内存上下文中（所有扩展对象
 * 都必须如此），并有如下控制结构。
 *
 * 若原始输入是常规"扁平"元组且尚未修改，扩展
 * 记录可含该元组。否则内容由 Datum/isnull 数组
 * 加类型信息表示。若为访问而拆解原元组但尚未
 * 改动，也可两种形式兼有。对按引用字段，此时
 * Datum 指向扁平元组内。一旦开始改字段，新的
 * 按引用字段在内存上下文中单独 palloc。
 * 见上。
 * 见上。
 *
 * 也可构建引用外部所存"扁平"元组的扩展记录，
 * 若调用方保证该元组在其寿命内不变。（主要为
 * 免去触发器函数中不必要的数据拷贝。）
 * 见上。
 */
#define ER_MAGIC 1384727874		/* 调试交叉检查用的 ID */

typedef struct ExpandedRecordHeader
{
	/* 扩展对象的标准头 */
	ExpandedObjectHeader hdr;

	/* 标识扩展记录的魔数（仅供调试） */
	int			er_magic;

	/* 各类标志位 */
	int			flags;
#define ER_FLAG_FVALUE_VALID	0x0001	/* fvalue 最新？ */
#define ER_FLAG_FVALUE_ALLOCED	0x0002	/* fvalue 本地存储？ */
#define ER_FLAG_DVALUES_VALID	0x0004	/* dvalues/dnulls 是否最新？ */
#define ER_FLAG_DVALUES_ALLOCED	0x0008	/* 有字段值在本地存储？ */
#define ER_FLAG_HAVE_EXTERNAL	0x0010	/* 有字段值为 external？ */
#define ER_FLAG_TUPDESC_ALLOCED	0x0020	/* tupdesc 本地存储？ */
#define ER_FLAG_IS_DOMAIN		0x0040	/* er_decltypeid 为域？ */
#define ER_FLAG_IS_DUMMY		0x0080	/* 此头为 dummy（见下） */
/* 替换元组数据时不得清除的标志位： */
#define ER_FLAGS_NON_DATA \
	(ER_FLAG_TUPDESC_ALLOCED | ER_FLAG_IS_DOMAIN | ER_FLAG_IS_DUMMY)

	/* 记录变量的声明类型（可为域类型） */
	Oid			er_decltypeid;

	/*
	 * 实际复合类型/typmod；从不为域（若 ER_FLAG_IS_DOMAIN，
	 * 则标识复合基类型）。将匹配 er_tupdesc 的
	 * tdtypeid/tdtypmod，以及本扩展记录制成或所存
	 * 复合 datum 的头字段。
	 */
	Oid			er_typeid;		/* 复合类型的类型 OID */
	int32		er_typmod;		/* 复合类型的 typmod */

	/*
	 * 若有元组描述符则在此，否则 NULL。可指向原属
	 * typcache 的引用计数 tupdesc，此时用上下文复位
	 * 回调释放引用计数。也可在本对象私有上下文中
	 * 本地分配（此时设 ER_FLAG_TUPDESC_ALLOCED）。
	 * 见上。
	 */
	TupleDesc	er_tupdesc;

	/*
	 * 进程内唯一的 tupdesc 标识（见 typcache.h）。此字段
	 * 从不等于 INVALID_TUPLEDESC_IDENTIFIER。
	 */
	uint64		er_tupdesc_id;

	/*
	 * 若有记录的 Datum 数组表示则保存在此；否则未
	 * 设 ER_FLAG_DVALUES_VALID，且若尚未分配则 dvalues/
	 * dnulls 可为 NULL。若已分配，则在私有上下文中
	 * palloc，长度匹配 er_tupdesc->natts。对按引用
	 * 字段，dvalues 可能指向 fstartptr..fendptr 区，
	 * 或指向单独 palloc 的块。
	 * 见上。
	 */
	Datum	   *dvalues;		/* Datum 数组 */
	bool	   *dnulls;			/* Datum 的空值标志数组 */
	int			nfields;		/* 上述数组长度 */

	/*
	 * flat_size 为扩展记录扁平等价物当前空间
	 * 需求(若已知)；否则为 0。存它使连续调用
	 * get_flat_size 便宜。若非 0，则 data_len、hoff、
	 * hasnull 也须有效。
	 */
	Size		flat_size;

	Size		data_len;		/* flat_size 内数据长 */
	int			hoff;			/* 头偏移 */
	bool		hasnull;		/* 需空值位图？ */

	/*
	 * 若有扁平表示则 fvalue 指向它，否则 NULL。若
	 * 扁平表示有效则设 FVALUE_VALID。即使因用户
	 * 字段变更而过时，仍可用于取系统列值。若有
	 * 扁平表示，则 fstartptr/fendptr 指向其数据区
	 * 起止+1；以便区分指向扁平内的 Datum 与单独
	 * palloc 的数据。
	 * 见上。
	 * 见上。
	 */
	HeapTuple	fvalue;			/* 可能是也可能不是私有存储 */
	char	   *fstartptr;		/* 其数据区起点 */
	char	   *fendptr;		/* 其数据区终点+1 */

	/* 某些对扩展记录的操作需要短命上下文 */
	MemoryContext er_short_term_cxt;	/* 短期内存上下文 */

	/* 域检查工作状态，ER_FLAG_IS_DOMAIN 时使用 */
	struct ExpandedRecordHeader *er_dummy_header;	/* 哑记录头 */
	void	   *er_domaininfo;	/* domain_check() 的缓存空间 */

	/* 回调信息（er_mcb.arg 非 NULL 时激活） */
	MemoryContextCallback er_mcb;
} ExpandedRecordHeader;

/* 扩展记录对象的 fmgr 函数与宏 */
static inline Datum
ExpandedRecordGetDatum(const ExpandedRecordHeader *erh)
{
	return EOHPGetRWDatum(&erh->hdr);
}

static inline Datum
ExpandedRecordGetRODatum(const ExpandedRecordHeader *erh)
{
	return EOHPGetRODatum(&erh->hdr);
}

#define PG_GETARG_EXPANDED_RECORD(n)  DatumGetExpandedRecord(PG_GETARG_DATUM(n))
#define PG_RETURN_EXPANDED_RECORD(x)  PG_RETURN_DATUM(ExpandedRecordGetDatum(x))

/* 其他各类宏 */
#define ExpandedRecordIsEmpty(erh) \
	(((erh)->flags & (ER_FLAG_DVALUES_VALID | ER_FLAG_FVALUE_VALID)) == 0)
#define ExpandedRecordIsDomain(erh) \
	(((erh)->flags & ER_FLAG_IS_DOMAIN) != 0)

/* 已有 eoh 时此可替代 TransferExpandedObject() */
#define TransferExpandedRecord(erh, cxt) \
	MemoryContextSetParent((erh)->hdr.eoh_context, cxt)

/* expanded_record_lookup_field() 返回的信息 */
typedef struct ExpandedRecordFieldInfo
{
	int			fnumber;		/* 记录中字段属性号 */
	Oid			ftypeid;		/* 字段类型/typmod */
	int32		ftypmod;
	Oid			fcollation;		/* 字段 collation */
} ExpandedRecordFieldInfo;

/*
 * expandedrecord.c 中所定义函数的原型
 */
extern ExpandedRecordHeader *make_expanded_record_from_typeid(Oid type_id, int32 typmod,
															  MemoryContext parentcontext);
extern ExpandedRecordHeader *make_expanded_record_from_tupdesc(TupleDesc tupdesc,
															   MemoryContext parentcontext);
extern ExpandedRecordHeader *make_expanded_record_from_exprecord(ExpandedRecordHeader *olderh,
																 MemoryContext parentcontext);
extern void expanded_record_set_tuple(ExpandedRecordHeader *erh,
									  HeapTuple tuple, bool copy, bool expand_external);
extern Datum make_expanded_record_from_datum(Datum recorddatum,
											 MemoryContext parentcontext);
extern TupleDesc expanded_record_fetch_tupdesc(ExpandedRecordHeader *erh);
extern HeapTuple expanded_record_get_tuple(ExpandedRecordHeader *erh);
extern ExpandedRecordHeader *DatumGetExpandedRecord(Datum d);
extern void deconstruct_expanded_record(ExpandedRecordHeader *erh);
extern bool expanded_record_lookup_field(ExpandedRecordHeader *erh,
										 const char *fieldname,
										 ExpandedRecordFieldInfo *finfo);
extern Datum expanded_record_fetch_field(ExpandedRecordHeader *erh, int fnumber,
										 bool *isnull);
extern void expanded_record_set_field_internal(ExpandedRecordHeader *erh,
											   int fnumber,
											   Datum newValue, bool isnull,
											   bool expand_external,
											   bool check_constraints);
extern void expanded_record_set_fields(ExpandedRecordHeader *erh,
									   const Datum *newValues, const bool *isnulls,
									   bool expand_external);

/* 外部代码切勿直接调用 expanded_record_set_field_internal */
#define expanded_record_set_field(erh, fnumber, newValue, isnull, expand_external) \
	expanded_record_set_field_internal(erh, fnumber, newValue, isnull, expand_external, true)

/*
 * 可内联的快路径。上面的 expanded_record_fetch_xxx
 * 处理一般情形。
 */

/* 获取扩展记录实际类型的 tupdesc */
static inline TupleDesc
expanded_record_get_tupdesc(ExpandedRecordHeader *erh)
{
	if (likely(erh->er_tupdesc != NULL))
		return erh->er_tupdesc;
	else
		return expanded_record_fetch_tupdesc(erh);
}

/* 获取记录字段的值 */
static inline Datum
expanded_record_get_field(ExpandedRecordHeader *erh, int fnumber,
						  bool *isnull)
{
	if ((erh->flags & ER_FLAG_DVALUES_VALID) &&
		likely(fnumber > 0 && fnumber <= erh->nfields))
	{
		*isnull = erh->dnulls[fnumber - 1];
		return erh->dvalues[fnumber - 1];
	}
	else
		return expanded_record_fetch_field(erh, fnumber, isnull);
}

#endif							/* EXPANDEDRECORD_H */
