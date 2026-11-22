/*-------------------------------------------------------------------------
 *
 * typcache.h
 *	  类型缓存定义。
 *
 * 类型缓存用于加速查找数据类型的某些信息，这些
 * 信息无法直接从类型的 pg_type 行获得。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/typcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPCACHE_H
#define TYPCACHE_H

#include "access/tupdesc.h"
#include "fmgr.h"
#include "storage/dsm.h"
#include "utils/dsa.h"


/* DomainConstraintCache 是仅 typcache.c 内可知的不透明结构。 */
typedef struct DomainConstraintCache DomainConstraintCache;

/* TypeCacheEnumData 是仅 typcache.c 内可知的不透明结构 */
struct TypeCacheEnumData;

typedef struct TypeCacheEntry
{
	/* typeId 是哈希查找键且 MUST BE FIRST */
	Oid			type_id;		/* 数据类型的 OID */

	uint32		type_id_hash;	/* OID 的哈希值 */

	/* 自 pg_type 行复制的若干附属信息 */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typstorage;
	char		typtype;
	Oid			typrelid;
	Oid			typsubscript;
	Oid			typelem;
	Oid			typcollation;

	/*
	 * 自 opfamily 条目获得的信息
	 *
	 * 若找不到匹配或尚未请求该信息，则为
	 * InvalidOid。另注意对数组与复合类型，
	 * typcache.c 在允许 eq_opr 等被设置前，会检查
	 * 所含类型是否可比较或可哈希。
	 */
	Oid			btree_opf;		/* the default btree opclass' family */
	Oid			btree_opintype; /* the default btree opclass' opcintype */
	Oid			hash_opf;		/* the default hash opclass' family */
	Oid			hash_opintype;	/* the default hash opclass' opcintype */
	Oid			eq_opr;			/* 相等运算符 */
	Oid			lt_opr;			/* 小于运算符 */
	Oid			gt_opr;			/* 大于运算符 */
	Oid			cmp_proc;		/* btree 比较函数 */
	Oid			hash_proc;		/* 哈希计算函数 */
	Oid			hash_extended_proc; /* 扩展哈希计算函数 */

	/*
	 * 为相等运算符、btree 比较函数与哈希计算函数
	 * 预先设好的 fmgr 调用信息。保存在类型缓存中，
	 * 以免 array_eq、array_cmp、hash_array 等反复调用时
	 * 内存泄漏。当前无需为 lt_opr 或 gt_opr 维护
	 * 调用信息。
	 */
	FmgrInfo	eq_opr_finfo;
	FmgrInfo	cmp_proc_finfo;
	FmgrInfo	hash_proc_finfo;
	FmgrInfo	hash_extended_proc_finfo;

	/*
	 * 若为复合类型（行类型）则为元组描述符，否则或
	 * 尚未请求则为 NULL。（注意：这是引用计数的
	 * tupledesc。）
	 *
	 * 为简化缓存依赖信息，tupDesc_identifier 是该
	 * tupledesc 在进程生命周期内唯一的标识，且在
	 * tupledesc 变化时改变。尚未确定则为零。
	 */
	TupleDesc	tupDesc;
	uint64		tupDesc_identifier;

	/*
	 * 请求 TYPECACHE_RANGE_INFO 时计算的字段。非范围
	 * 类型或尚未请求则为零。注意 rng_cmp_proc_finfo
	 * 可能与元素类型的默认 btree 比较函数不同。
	 * 见上。
	 */
	struct TypeCacheEntry *rngelemtype; /* 范围元素类型 */
	Oid			rng_collation;	/* 比较用 collation（若有） */
	FmgrInfo	rng_cmp_proc_finfo; /* 比较函数 */
	FmgrInfo	rng_canonical_finfo;	/* 规范化函数（若有） */
	FmgrInfo	rng_subdiff_finfo;	/* 差值函数（若有） */

	/*
	 * 请求 TYPECACHE_MULTIRANGE_INFO 时计算的字段。
	 */
	struct TypeCacheEntry *rngtype; /* 多重范围的底层范围类型 */

	/*
	 * 若为域类型则为域的基类型与 typmod。非域或
	 * 尚未请求则为零。
	 */
	Oid			domainBaseType;
	int32		domainBaseTypmod;

	/*
	 * 若为域类型则为域约束数据。非域、无约束或
	 * 尚未请求则为 NULL。
	 */
	DomainConstraintCache *domainData;

	/* 私有数据，仅供 typcache.c 内部使用 */
	int			flags;			/* 关于已计算内容的标志 */

	/*
	 * 枚举类型的私有信息。非枚举或尚未请求则为
	 * NULL。
	 */
	struct TypeCacheEnumData *enumData;

	/* 还维护所有已知域类型缓存条目的列表 */
	struct TypeCacheEntry *nextDomain;
} TypeCacheEntry;

/* 位标志，表明调用方需要哪些字段被设置 */
#define TYPECACHE_EQ_OPR			0x00001
#define TYPECACHE_LT_OPR			0x00002
#define TYPECACHE_GT_OPR			0x00004
#define TYPECACHE_CMP_PROC			0x00008
#define TYPECACHE_HASH_PROC			0x00010
#define TYPECACHE_EQ_OPR_FINFO		0x00020
#define TYPECACHE_CMP_PROC_FINFO	0x00040
#define TYPECACHE_HASH_PROC_FINFO	0x00080
#define TYPECACHE_TUPDESC			0x00100
#define TYPECACHE_BTREE_OPFAMILY	0x00200
#define TYPECACHE_HASH_OPFAMILY		0x00400
#define TYPECACHE_RANGE_INFO		0x00800
#define TYPECACHE_DOMAIN_BASE_INFO			0x01000
#define TYPECACHE_DOMAIN_CONSTR_INFO		0x02000
#define TYPECACHE_HASH_EXTENDED_PROC		0x04000
#define TYPECACHE_HASH_EXTENDED_PROC_FINFO	0x08000
#define TYPECACHE_MULTIRANGE_INFO			0x10000

/* 此值不等于任何有效 tupledesc 标识，也不为 0 */
#define INVALID_TUPLEDESC_IDENTIFIER ((uint64) 1)

/*
 * 欲长期持有域约束集引用的调用方须存于此。用
 * InitDomainConstraintRef() 与 UpdateDomainConstraintRef()
 * 管理。注意：DomainConstraintState 视为可执行表达式
 * 类型，故定义在 execnodes.h。
 */
typedef struct DomainConstraintRef
{
	List	   *constraints;	/* DomainConstraintState 节点列表 */
	MemoryContext refctx;		/* 持有 DomainConstraintRef */
	TypeCacheEntry *tcache;		/* 域类型的 typcache 条目 */
	bool		need_exprstate; /* 需 check_exprstate？ */

	/* 管理数据 --- 将这些字段视为 typcache.c 私有 */
	DomainConstraintCache *dcc; /* 当前约束，无则为 NULL */
	MemoryContextCallback callback; /* 完成后用于释放引用计数 */
} DomainConstraintRef;

typedef struct SharedRecordTypmodRegistry SharedRecordTypmodRegistry;

extern TypeCacheEntry *lookup_type_cache(Oid type_id, int flags);

extern void InitDomainConstraintRef(Oid type_id, DomainConstraintRef *ref,
									MemoryContext refctx, bool need_exprstate);

extern void UpdateDomainConstraintRef(DomainConstraintRef *ref);

extern bool DomainHasConstraints(Oid type_id);

extern TupleDesc lookup_rowtype_tupdesc(Oid type_id, int32 typmod);

extern TupleDesc lookup_rowtype_tupdesc_noerror(Oid type_id, int32 typmod,
												bool noError);

extern TupleDesc lookup_rowtype_tupdesc_copy(Oid type_id, int32 typmod);

extern TupleDesc lookup_rowtype_tupdesc_domain(Oid type_id, int32 typmod,
											   bool noError);

extern void assign_record_type_typmod(TupleDesc tupDesc);

extern uint64 assign_record_type_identifier(Oid type_id, int32 typmod);

extern int	compare_values_of_enum(TypeCacheEntry *tcache, Oid arg1, Oid arg2);

extern size_t SharedRecordTypmodRegistryEstimate(void);

extern void SharedRecordTypmodRegistryInit(SharedRecordTypmodRegistry *,
										   dsm_segment *segment, dsa_area *area);

extern void SharedRecordTypmodRegistryAttach(SharedRecordTypmodRegistry *);

#endif							/* TYPCACHE_H */
