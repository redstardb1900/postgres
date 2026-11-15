/*-------------------------------------------------------------------------
 *
 * typcache.c
 *	  类型缓存代码
 *
 * 类型缓存可加速查询某些信息，这类信息无法从
 * 类型的 pg_type 行中直接获得。例如，我们用类型
 * 默认的 btree 算子类，若无 btree 算子类则用
 * 默认的哈希算子类，以确定分组和排序该类型时
 * 应使用的算子（GROUP BY、ORDER BY ASC/DESC）。
 *
 * 这里有几处看似奇怪的设计选择，是为了让
 * 通用数组和记录处理例程（如 array_eq()、
 * record_cmp() 和 hash_array()）能使用类型缓存。
 * 因为它们被用作索引支持算子，不能泄漏内
 * 存。为了高效执行，凡跨调用需复用的信息
 * 在类型缓存中。
 *
 * 类型缓存条目一旦创建，就会存活到后端退出为止，
 * 因此无需调用接口来释放缓存条目。若类型被删除，
 * 缓存条目沦为废弃存储。此情形不常发生，而假定
 * typcache 条目永远有效，则允许在长寿场所缓存指向
 * 它们的指针。
 *
 * 我们提供了一些手段，可在存储数据过时之际更新缓
 * 存条目。从 pg_type 行提取的核心数据会在检测到
 * pg_type 更新时予以更新。依赖算子类的信息则在检测到
 * pg_opclass 更新时清除。我们还支持清除行类型缓存条
 * 目的元组描述符部分与算子/函数部分，因为这些部分
 * 可能因 ALTER TABLE 而需要变更。域约束变更也会得到
 * 妥善跟踪。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/typcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/session.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_range.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "lib/dshash.h"
#include "optimizer/optimizer.h"
#include "port/pg_bitutils.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


/* lookup_type_cache 搜索的主类型缓存哈希表 */
static HTAB *TypeCacheHash = NULL;

/* 各域类型的类型缓存条目列表 */
static TypeCacheEntry *firstDomainTypeEntry = NULL;

/* TypeCacheEntry.flags 字段中的私有标志位 */
#define TCFLAGS_HAVE_PG_TYPE_DATA			0x000001
#define TCFLAGS_CHECKED_BTREE_OPCLASS		0x000002
#define TCFLAGS_CHECKED_HASH_OPCLASS		0x000004
#define TCFLAGS_CHECKED_EQ_OPR				0x000008
#define TCFLAGS_CHECKED_LT_OPR				0x000010
#define TCFLAGS_CHECKED_GT_OPR				0x000020
#define TCFLAGS_CHECKED_CMP_PROC			0x000040
#define TCFLAGS_CHECKED_HASH_PROC			0x000080
#define TCFLAGS_CHECKED_HASH_EXTENDED_PROC	0x000100
#define TCFLAGS_CHECKED_ELEM_PROPERTIES		0x000200
#define TCFLAGS_HAVE_ELEM_EQUALITY			0x000400
#define TCFLAGS_HAVE_ELEM_COMPARE			0x000800
#define TCFLAGS_HAVE_ELEM_HASHING			0x001000
#define TCFLAGS_HAVE_ELEM_EXTENDED_HASHING	0x002000
#define TCFLAGS_CHECKED_FIELD_PROPERTIES	0x004000
#define TCFLAGS_HAVE_FIELD_EQUALITY			0x008000
#define TCFLAGS_HAVE_FIELD_COMPARE			0x010000
#define TCFLAGS_HAVE_FIELD_HASHING			0x020000
#define TCFLAGS_HAVE_FIELD_EXTENDED_HASHING	0x040000
#define TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS	0x080000
#define TCFLAGS_DOMAIN_BASE_IS_COMPOSITE	0x100000

/* 相等/比较/哈希相关标志为这些之外的全部： */
#define TCFLAGS_OPERATOR_FLAGS \
	(~(TCFLAGS_HAVE_PG_TYPE_DATA | \
	   TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS | \
	   TCFLAGS_DOMAIN_BASE_IS_COMPOSITE))

/*
 * 关于域类型约束的存储数据。注意对于无约束域这一
 * 常见情形我们并不创建此结构体；只需把 domainData
 * 置为 NULL 来表示这一点。
 *
 * 在 DomainConstraintCache 内部，我们存储表达式计
 * 划树，但 DomainConstraintState 节点的 check_exprstate
 * 字段则只是 NULL。需要时，表达式求值节点通过平
 * 面复制 DomainConstraintState 节点并对 check_expr
 * 应用 ExecInitExpr 而来。这样的节点树并不属于此缓
 * 存，而视为属于 DomainConstraintRef。
 */
struct DomainConstraintCache
{
	List	   *constraints;	/* DomainConstraintState 节点列表 */
	MemoryContext dccContext;	/* 保存全部相关数据的内存上下文 */
	long		dccRefCount;	/* 本结构体的引用数 */
};

/* 支持枚举值比较的私有信息 */
typedef struct
{
	Oid			enum_oid;		/* 枚举值的 OID */
	float4		sort_order;		/* 其排序位置 */
} EnumItem;

typedef struct TypeCacheEnumData
{
	Oid			bitmap_base;	/* 对应 bitmapset 位 0 的 OID */
	Bitmapset  *sorted_values;	/* 已知有序的 OID 集合 */
	int			num_values;		/* 枚举中的值总数 */
	EnumItem	enum_values[FLEXIBLE_ARRAY_MEMBER];
} TypeCacheEnumData;

/*
 * 我们用单独一张表存储非匿名记录类型的定义。
 * 一旦定义，记录类型将在后端生存期内一直被记住。
 * 对"同一"记录类型（"同一"指 equalTupleDescs）的
 * 后续使用将指向已有的表条目。
 *
 * 已存储的记录类型记录于一个 TupleDesc 线性数组中
 * 可用分配的 typmod 快速索引。此外还有一个哈希表，
 * 用来加速对匹配 TupleDesc 的搜索。
 */

typedef struct RecordCacheEntry
{
	TupleDesc	tupdesc;
} RecordCacheEntry;

/*
 * 为了处理并行查询中各后端交换的非匿名记录类型，
 * 我们还需要上述结构的共享版本。
 */
struct SharedRecordTypmodRegistry
{
	/* 用于查找匹配 TupleDesc 的哈希表。 */
	dshash_table_handle record_table_handle;
	/* 用于按 typmod 查找 TupleDesc 的哈希表 */
	dshash_table_handle typmod_table_handle;
	/* 新记录 typmod 编号的来源。 */
	pg_atomic_uint32 next_typmod;
};

/*
 * 用共享元组描述符作哈希表键时，我们需要能以一个
 * 后端本地的 TupleDesc 搜索相等共享 TupleDesc 的途
 * 径。为此我们使用这一类型，它既能容纳两者，其哈希
 * 及比较函数均能处理两者。
 */
typedef struct SharedRecordTableKey
{
	union
	{
		TupleDesc	local_tupdesc;
		dsa_pointer shared_tupdesc;
	}			u;
	bool		shared;
} SharedRecordTableKey;

/*
 * RecordCacheEntry 的共享版本。它让我们能以本地或
 * 共享内存中的 TupleDesc 查找 typmod。
 */
typedef struct SharedRecordTableEntry
{
	SharedRecordTableKey key;
} SharedRecordTableEntry;

/*
 * SharedRecordTypmodRegistry 的 typmod 表条目。可以
 * 用 typmod 查共享内存的 TupleDesc。
 */
typedef struct SharedTypmodTableEntry
{
	uint32		typmod;
	dsa_pointer shared_tupdesc;
} SharedTypmodTableEntry;

/*
 * 用于 SharedRecordTableKey 的比较函数。
 */
static int
shared_record_table_compare(const void *a, const void *b, size_t size,
							void *arg)
{
	dsa_area   *area = (dsa_area *) arg;
	SharedRecordTableKey *k1 = (SharedRecordTableKey *) a;
	SharedRecordTableKey *k2 = (SharedRecordTableKey *) b;
	TupleDesc	t1;
	TupleDesc	t2;

	if (k1->shared)
		t1 = (TupleDesc) dsa_get_address(area, k1->u.shared_tupdesc);
	else
		t1 = k1->u.local_tupdesc;

	if (k2->shared)
		t2 = (TupleDesc) dsa_get_address(area, k2->u.shared_tupdesc);
	else
		t2 = k2->u.local_tupdesc;

	return equalTupleDescs(t1, t2) ? 0 : 1;
}

/*
 * SharedRecordTableKey 的哈希函数。
 */
static uint32
shared_record_table_hash(const void *a, size_t size, void *arg)
{
	dsa_area   *area = (dsa_area *) arg;
	SharedRecordTableKey *k = (SharedRecordTableKey *) a;
	TupleDesc	t;

	if (k->shared)
		t = (TupleDesc) dsa_get_address(area, k->u.shared_tupdesc);
	else
		t = k->u.local_tupdesc;

	return hashTupleDesc(t);
}

/* SharedRecordTypmodRegistry 的 TupleDesc 表参数 */
static const dshash_parameters srtr_record_table_params = {
	sizeof(SharedRecordTableKey),	/* 未用 */
	sizeof(SharedRecordTableEntry),
	shared_record_table_compare,
	shared_record_table_hash,
	LWTRANCHE_PER_SESSION_RECORD_TYPE
};

/* SharedRecordTypmodRegistry 的 typmod 哈希表参数 */
static const dshash_parameters srtr_typmod_table_params = {
	sizeof(uint32),
	sizeof(SharedTypmodTableEntry),
	dshash_memcmp,
	dshash_memhash,
	LWTRANCHE_PER_SESSION_RECORD_TYPMOD
};

/* 用于识别已注册记录类型的哈希表 */
static HTAB *RecordCacheHash = NULL;

typedef struct RecordCacheArrayEntry
{
	uint64		id;
	TupleDesc	tupdesc;
} RecordCacheArrayEntry;

/* 已注册记录类型的信息数组，按所分配的 typmod 索引 */
static RecordCacheArrayEntry *RecordCacheArray = NULL;
static int32 RecordCacheArrayLen = 0;	/* 上述数组已分配的长度 */
static int32 NextRecordTypmod = 0;	/* 已用条目数 */

/*
 * 生成唯一元组描述符标识符的进程级计数器。
 * 零和一（INVALID_TUPLEDESC_IDENTIFIER）不允许被选
 * 作标识符，故计数器从 INVALID_TUPLEDESC_IDENTIFIER 开始。
 */
static uint64 tupledesc_id_counter = INVALID_TUPLEDESC_IDENTIFIER;

static void load_typcache_tupdesc(TypeCacheEntry *typentry);
static void load_rangetype_info(TypeCacheEntry *typentry);
static void load_multirangetype_info(TypeCacheEntry *typentry);
static void load_domaintype_info(TypeCacheEntry *typentry);
static int	dcs_cmp(const void *a, const void *b);
static void decr_dcc_refcount(DomainConstraintCache *dcc);
static void dccref_deletion_callback(void *arg);
static List *prep_domain_constraints(List *constraints, MemoryContext execctx);
static bool array_element_has_equality(TypeCacheEntry *typentry);
static bool array_element_has_compare(TypeCacheEntry *typentry);
static bool array_element_has_hashing(TypeCacheEntry *typentry);
static bool array_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_array_element_properties(TypeCacheEntry *typentry);
static bool record_fields_have_equality(TypeCacheEntry *typentry);
static bool record_fields_have_compare(TypeCacheEntry *typentry);
static bool record_fields_have_hashing(TypeCacheEntry *typentry);
static bool record_fields_have_extended_hashing(TypeCacheEntry *typentry);
static void cache_record_field_properties(TypeCacheEntry *typentry);
static bool range_element_has_hashing(TypeCacheEntry *typentry);
static bool range_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_range_element_properties(TypeCacheEntry *typentry);
static bool multirange_element_has_hashing(TypeCacheEntry *typentry);
static bool multirange_element_has_extended_hashing(TypeCacheEntry *typentry);
static void cache_multirange_element_properties(TypeCacheEntry *typentry);
static void TypeCacheRelCallback(Datum arg, Oid relid);
static void TypeCacheTypCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue);
static void TypeCacheConstrCallback(Datum arg, int cacheid, uint32 hashvalue);
static void load_enum_cache_data(TypeCacheEntry *tcache);
static EnumItem *find_enumitem(TypeCacheEnumData *enumdata, Oid arg);
static int	enum_oid_cmp(const void *left, const void *right);
static void shared_record_typmod_registry_detach(dsm_segment *segment,
												 Datum datum);
static TupleDesc find_or_make_matching_shared_tupledesc(TupleDesc tupdesc);
static dsa_pointer share_tupledesc(dsa_area *area, TupleDesc tupdesc,
								   uint32 typmod);


/*
 * lookup_type_cache
 *
 * 获取指定数据类型的类型缓存条目，并确保 'flags' 中
 * 各比特请求的所有字段都有效。
 *
 * 结果永不为 NULL --- 如果传入的类型 OID 无效就
 * 会 ereport()。但注意，'flags' 请求的一个或多个值
 * 可能找不到；调用者需检查这些字段是否为
 * InvalidOid。
 */
TypeCacheEntry *
lookup_type_cache(Oid type_id, int flags)
{
	TypeCacheEntry *typentry;
	bool		found;

	if (TypeCacheHash == NULL)
	{
		/* 首次进入：初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TypeCacheEntry);
		TypeCacheHash = hash_create("Type information cache", 64,
									&ctl, HASH_ELEM | HASH_BLOBS);

		/* 同时为 SI 失效建立回调 */
		CacheRegisterRelcacheCallback(TypeCacheRelCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(TYPEOID, TypeCacheTypCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(CLAOID, TypeCacheOpcCallback, (Datum) 0);
		CacheRegisterSyscacheCallback(CONSTROID, TypeCacheConstrCallback, (Datum) 0);

		/* 同时确保 CacheMemoryContext 存在 */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/* 尝试查找已有条目 */
	typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
											  &type_id,
											  HASH_FIND, NULL);
	if (typentry == NULL)
	{
		/*
		 * 若没找到，就要新建一个。但先要查找 pg_type 行，
		 * 以免给无效的类型 OID 创建缓存条目。如果类
		 * 型 OID 无效，则报面向用户的错误，因为
		 * 有些代码路径（如 domain_in()）允许以用户提供
		 * 的 OID 到达本函数。
		 */
		HeapTuple	tp;
		Form_pg_type typtup;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
		if (!HeapTupleIsValid(tp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type with OID %u does not exist", type_id)));
		typtup = (Form_pg_type) GETSTRUCT(tp);
		if (!typtup->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							NameStr(typtup->typname))));

		/* 现在建 typcache 条目 */
		typentry = (TypeCacheEntry *) hash_search(TypeCacheHash,
												  &type_id,
												  HASH_ENTER, &found);
		Assert(!found);			/* 刚才它还不在这里 */

		MemSet(typentry, 0, sizeof(TypeCacheEntry));

		/* 根据定义这些字段永不变更 */
		typentry->type_id = type_id;
		typentry->type_id_hash = GetSysCacheHashValue1(TYPEOID,
													   ObjectIdGetDatum(type_id));

		/* 保持此段与下方代码同步 */
		typentry->typlen = typtup->typlen;
		typentry->typbyval = typtup->typbyval;
		typentry->typalign = typtup->typalign;
		typentry->typstorage = typtup->typstorage;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;
		typentry->typsubscript = typtup->typsubscript;
		typentry->typelem = typtup->typelem;
		typentry->typcollation = typtup->typcollation;
		typentry->flags |= TCFLAGS_HAVE_PG_TYPE_DATA;

		/* 若是域类型，立即把它串接到域缓存列表中 */
		if (typentry->typtype == TYPTYPE_DOMAIN)
		{
			typentry->nextDomain = firstDomainTypeEntry;
			firstDomainTypeEntry = typentry;
		}

		ReleaseSysCache(tp);
	}
	else if (!(typentry->flags & TCFLAGS_HAVE_PG_TYPE_DATA))
	{
		/*
		 * 已有条目，但其 pg_type 行已变更，所以重新加
		 * 载直接得自 pg_type 的数据。
		 */
		HeapTuple	tp;
		Form_pg_type typtup;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_id));
		if (!HeapTupleIsValid(tp))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type with OID %u does not exist", type_id)));
		typtup = (Form_pg_type) GETSTRUCT(tp);
		if (!typtup->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("type \"%s\" is only a shell",
							NameStr(typtup->typname))));

		/*
		 * 保持此段与上方代码同步。多数字段不应改变（
		 * 尤其是 typtype），但仍照样复制。
		 */
		typentry->typlen = typtup->typlen;
		typentry->typbyval = typtup->typbyval;
		typentry->typalign = typtup->typalign;
		typentry->typstorage = typtup->typstorage;
		typentry->typtype = typtup->typtype;
		typentry->typrelid = typtup->typrelid;
		typentry->typsubscript = typtup->typsubscript;
		typentry->typelem = typtup->typelem;
		typentry->typcollation = typtup->typcollation;
		typentry->flags |= TCFLAGS_HAVE_PG_TYPE_DATA;

		ReleaseSysCache(tp);
	}

	/*
	 * 如果尚未查找算子类且依赖信息被请求，则查
	 * 找。
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
				  TYPECACHE_CMP_PROC |
				  TYPECACHE_EQ_OPR_FINFO | TYPECACHE_CMP_PROC_FINFO |
				  TYPECACHE_BTREE_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_BTREE_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, BTREE_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->btree_opf = get_opclass_family(opclass);
			typentry->btree_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->btree_opf = typentry->btree_opintype = InvalidOid;
		}

		/*
		 * 重置由 btree 算子类导出的信息。特别要注意，
		 * 即使此前已找到 eq_opr 我们也会重新判定它；这样
		 * 做是为了应对类型此前只有哈希算子类、后来又
		 * 加了 btree 算子类的情况。
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_EQ_OPR |
							 TCFLAGS_CHECKED_LT_OPR |
							 TCFLAGS_CHECKED_GT_OPR |
							 TCFLAGS_CHECKED_CMP_PROC);
		typentry->flags |= TCFLAGS_CHECKED_BTREE_OPCLASS;
	}

	/*
	 * 如果需要查找相等算子但没有 btree 算子类，则强制
	 * 查找哈希算子类。
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR) &&
		typentry->btree_opf == InvalidOid)
		flags |= TYPECACHE_HASH_OPFAMILY;

	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO |
				  TYPECACHE_HASH_EXTENDED_PROC |
				  TYPECACHE_HASH_EXTENDED_PROC_FINFO |
				  TYPECACHE_HASH_OPFAMILY)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_OPCLASS))
	{
		Oid			opclass;

		opclass = GetDefaultOpClass(type_id, HASH_AM_OID);
		if (OidIsValid(opclass))
		{
			typentry->hash_opf = get_opclass_family(opclass);
			typentry->hash_opintype = get_opclass_input_type(opclass);
		}
		else
		{
			typentry->hash_opf = typentry->hash_opintype = InvalidOid;
		}

		/*
		 * 重置从哈希算子类导出的信息。我们*不会*重置
		 * eq_opr；如果已从 btree 算子类找到一个，那么
		 * 该决定仍有效。
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC |
							 TCFLAGS_CHECKED_HASH_EXTENDED_PROC);
		typentry->flags |= TCFLAGS_CHECKED_HASH_OPCLASS;
	}

	/*
	 * 如果尚未查找，则查找所请求的算子和函数。
	 */
	if ((flags & (TYPECACHE_EQ_OPR | TYPECACHE_EQ_OPR_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_EQ_OPR))
	{
		Oid			eq_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTEqualStrategyNumber);
		if (eq_opr == InvalidOid &&
			typentry->hash_opf != InvalidOid)
			eq_opr = get_opfamily_member(typentry->hash_opf,
										 typentry->hash_opintype,
										 typentry->hash_opintype,
										 HTEqualStrategyNumber);

		/*
		 * 如果拟用的相等算子是 array_eq 或 record_eq，则检查
		 * 元素类型或各列类型是否支持相等。若不支持，
		 * array_eq 或 record_eq 将在运行时失败，所以我们
		 * 不想报告该类型有相等。（范围和多范围可
		 * 省类似检查，因为范围只有在子类型支持相等
		 * 时才可能被创建。）
		 */
		if (eq_opr == ARRAY_EQ_OP &&
			!array_element_has_equality(typentry))
			eq_opr = InvalidOid;
		else if (eq_opr == RECORD_EQ_OP &&
				 !record_fields_have_equality(typentry))
			eq_opr = InvalidOid;

		/* 仅在改变状态时才强制更新 eq_opr_finfo */
		if (typentry->eq_opr != eq_opr)
			typentry->eq_opr_finfo.fn_oid = InvalidOid;

		typentry->eq_opr = eq_opr;

		/*
		 * 每当获得相等算子的新信息时，我们就重置关于
		 * 哈希函数的信息。这样能确保哈希
		 * 函数与算子匹配。
		 */
		typentry->flags &= ~(TCFLAGS_CHECKED_HASH_PROC |
							 TCFLAGS_CHECKED_HASH_EXTENDED_PROC);
		typentry->flags |= TCFLAGS_CHECKED_EQ_OPR;
	}
	if ((flags & TYPECACHE_LT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_LT_OPR))
	{
		Oid			lt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			lt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTLessStrategyNumber);

		/*
		 * 同上，确保 array_cmp 或 record_cmp 能成功；但同样
		 * 无需对范围或多范围做专门检查。
		 */
		if (lt_opr == ARRAY_LT_OP &&
			!array_element_has_compare(typentry))
			lt_opr = InvalidOid;
		else if (lt_opr == RECORD_LT_OP &&
				 !record_fields_have_compare(typentry))
			lt_opr = InvalidOid;

		typentry->lt_opr = lt_opr;
		typentry->flags |= TCFLAGS_CHECKED_LT_OPR;
	}
	if ((flags & TYPECACHE_GT_OPR) &&
		!(typentry->flags & TCFLAGS_CHECKED_GT_OPR))
	{
		Oid			gt_opr = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			gt_opr = get_opfamily_member(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTGreaterStrategyNumber);

		/*
		 * 同上，确保 array_cmp 或 record_cmp 能成功；但同样
		 * 无需对范围或多范围做专门检查。
		 */
		if (gt_opr == ARRAY_GT_OP &&
			!array_element_has_compare(typentry))
			gt_opr = InvalidOid;
		else if (gt_opr == RECORD_GT_OP &&
				 !record_fields_have_compare(typentry))
			gt_opr = InvalidOid;

		typentry->gt_opr = gt_opr;
		typentry->flags |= TCFLAGS_CHECKED_GT_OPR;
	}
	if ((flags & (TYPECACHE_CMP_PROC | TYPECACHE_CMP_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_CMP_PROC))
	{
		Oid			cmp_proc = InvalidOid;

		if (typentry->btree_opf != InvalidOid)
			cmp_proc = get_opfamily_proc(typentry->btree_opf,
										 typentry->btree_opintype,
										 typentry->btree_opintype,
										 BTORDER_PROC);

		/*
		 * 同上，确保 array_cmp 或 record_cmp 能成功；但同样
		 * 无需对范围或多范围做专门检查。
		 */
		if (cmp_proc == F_BTARRAYCMP &&
			!array_element_has_compare(typentry))
			cmp_proc = InvalidOid;
		else if (cmp_proc == F_BTRECORDCMP &&
				 !record_fields_have_compare(typentry))
			cmp_proc = InvalidOid;

		/* 仅在改变状态时才强制更新 cmp_proc_finfo */
		if (typentry->cmp_proc != cmp_proc)
			typentry->cmp_proc_finfo.fn_oid = InvalidOid;

		typentry->cmp_proc = cmp_proc;
		typentry->flags |= TCFLAGS_CHECKED_CMP_PROC;
	}
	if ((flags & (TYPECACHE_HASH_PROC | TYPECACHE_HASH_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_PROC))
	{
		Oid			hash_proc = InvalidOid;

		/*
		 * 我们坚持：若已判定出 eq_opr，它必须与哈希算
		 * 子类匹配；否则报告没有哈希函数。
		 */
		if (typentry->hash_opf != InvalidOid &&
			(!OidIsValid(typentry->eq_opr) ||
			 typentry->eq_opr == get_opfamily_member(typentry->hash_opf,
													 typentry->hash_opintype,
													 typentry->hash_opintype,
													 HTEqualStrategyNumber)))
			hash_proc = get_opfamily_proc(typentry->hash_opf,
										  typentry->hash_opintype,
										  typentry->hash_opintype,
										  HASHSTANDARD_PROC);

		/*
		 * 同上，确保 hash_array、hash_record 或 hash_range 能
		 * 成功
		 */
		if (hash_proc == F_HASH_ARRAY &&
			!array_element_has_hashing(typentry))
			hash_proc = InvalidOid;
		else if (hash_proc == F_HASH_RECORD &&
				 !record_fields_have_hashing(typentry))
			hash_proc = InvalidOid;
		else if (hash_proc == F_HASH_RANGE &&
				 !range_element_has_hashing(typentry))
			hash_proc = InvalidOid;

		/*
		 * hash_multirange 同理。
		 */
		if (hash_proc == F_HASH_MULTIRANGE &&
			!multirange_element_has_hashing(typentry))
			hash_proc = InvalidOid;

		/* 仅在改变状态时才强制更新 hash_proc_finfo */
		if (typentry->hash_proc != hash_proc)
			typentry->hash_proc_finfo.fn_oid = InvalidOid;

		typentry->hash_proc = hash_proc;
		typentry->flags |= TCFLAGS_CHECKED_HASH_PROC;
	}
	if ((flags & (TYPECACHE_HASH_EXTENDED_PROC |
				  TYPECACHE_HASH_EXTENDED_PROC_FINFO)) &&
		!(typentry->flags & TCFLAGS_CHECKED_HASH_EXTENDED_PROC))
	{
		Oid			hash_extended_proc = InvalidOid;

		/*
		 * 我们坚持：若已判定出 eq_opr，它必须与哈希算
		 * 子类匹配；否则报告没有哈希函数。
		 */
		if (typentry->hash_opf != InvalidOid &&
			(!OidIsValid(typentry->eq_opr) ||
			 typentry->eq_opr == get_opfamily_member(typentry->hash_opf,
													 typentry->hash_opintype,
													 typentry->hash_opintype,
													 HTEqualStrategyNumber)))
			hash_extended_proc = get_opfamily_proc(typentry->hash_opf,
												   typentry->hash_opintype,
												   typentry->hash_opintype,
												   HASHEXTENDED_PROC);

		/*
		 * 同上，确保 hash_array_extended、hash_record_extended 或
		 * hash_range_extended 能成功。
		 */
		if (hash_extended_proc == F_HASH_ARRAY_EXTENDED &&
			!array_element_has_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;
		else if (hash_extended_proc == F_HASH_RECORD_EXTENDED &&
				 !record_fields_have_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;
		else if (hash_extended_proc == F_HASH_RANGE_EXTENDED &&
				 !range_element_has_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;

		/*
		 * hash_multirange_extended 同理。
		 */
		if (hash_extended_proc == F_HASH_MULTIRANGE_EXTENDED &&
			!multirange_element_has_extended_hashing(typentry))
			hash_extended_proc = InvalidOid;

		/* 仅在改变状态时才强制更新 proc 的 finfo */
		if (typentry->hash_extended_proc != hash_extended_proc)
			typentry->hash_extended_proc_finfo.fn_oid = InvalidOid;

		typentry->hash_extended_proc = hash_extended_proc;
		typentry->flags |= TCFLAGS_CHECKED_HASH_EXTENDED_PROC;
	}

	/*
	 * 按请求建立 fmgr 查找信息
	 *
	 * 注：我们告诉 fmgr：finfo 结构体位于 CacheMemoryContext
	 * 中，这并不十分准确（它们其实位于哈希表私有内
	 * 存上下文），但对我们的用途够用。
	 *
	 * 注：上面的代码避免使 finfo 结构体失效，除非所
	 * 引用的算子/函数 OID 真正变更。这是为了防止
	 * 附加在 finfo 上的附属数据发生不必要的泄漏，因为
	 * 那会造成会话生存期的内存泄漏。
	 */
	if ((flags & TYPECACHE_EQ_OPR_FINFO) &&
		typentry->eq_opr_finfo.fn_oid == InvalidOid &&
		typentry->eq_opr != InvalidOid)
	{
		Oid			eq_opr_func;

		eq_opr_func = get_opcode(typentry->eq_opr);
		if (eq_opr_func != InvalidOid)
			fmgr_info_cxt(eq_opr_func, &typentry->eq_opr_finfo,
						  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_CMP_PROC_FINFO) &&
		typentry->cmp_proc_finfo.fn_oid == InvalidOid &&
		typentry->cmp_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->cmp_proc, &typentry->cmp_proc_finfo,
					  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_HASH_PROC_FINFO) &&
		typentry->hash_proc_finfo.fn_oid == InvalidOid &&
		typentry->hash_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->hash_proc, &typentry->hash_proc_finfo,
					  CacheMemoryContext);
	}
	if ((flags & TYPECACHE_HASH_EXTENDED_PROC_FINFO) &&
		typentry->hash_extended_proc_finfo.fn_oid == InvalidOid &&
		typentry->hash_extended_proc != InvalidOid)
	{
		fmgr_info_cxt(typentry->hash_extended_proc,
					  &typentry->hash_extended_proc_finfo,
					  CacheMemoryContext);
	}

	/*
	 * 如果是复合类型（行类型），按需获取 tupdesc
	 */
	if ((flags & TYPECACHE_TUPDESC) &&
		typentry->tupDesc == NULL &&
		typentry->typtype == TYPTYPE_COMPOSITE)
	{
		load_typcache_tupdesc(typentry);
	}

	/*
	 * 若被请求，则获取范围类型的信息
	 *
	 * 这包括确保关于范围元素类型的基本信息是最
	 * 新的。
	 */
	if ((flags & TYPECACHE_RANGE_INFO) &&
		typentry->typtype == TYPTYPE_RANGE)
	{
		if (typentry->rngelemtype == NULL)
			load_rangetype_info(typentry);
		else if (!(typentry->rngelemtype->flags & TCFLAGS_HAVE_PG_TYPE_DATA))
			(void) lookup_type_cache(typentry->rngelemtype->type_id, 0);
	}

	/*
	 * 若被请求，则获取多范围类型的信息
	 */
	if ((flags & TYPECACHE_MULTIRANGE_INFO) &&
		typentry->rngtype == NULL &&
		typentry->typtype == TYPTYPE_MULTIRANGE)
	{
		load_multirangetype_info(typentry);
	}

	/*
	 * 若被请求，则获取域类型的信息
	 */
	if ((flags & TYPECACHE_DOMAIN_BASE_INFO) &&
		typentry->domainBaseType == InvalidOid &&
		typentry->typtype == TYPTYPE_DOMAIN)
	{
		typentry->domainBaseTypmod = -1;
		typentry->domainBaseType =
			getBaseTypeAndTypmod(type_id, &typentry->domainBaseTypmod);
	}
	if ((flags & TYPECACHE_DOMAIN_CONSTR_INFO) &&
		(typentry->flags & TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS) == 0 &&
		typentry->typtype == TYPTYPE_DOMAIN)
	{
		load_domaintype_info(typentry);
	}

	return typentry;
}

/*
 * load_typcache_tupdesc --- 用于设置复合类型 tupDesc 的辅助例程
 */
static void
load_typcache_tupdesc(TypeCacheEntry *typentry)
{
	Relation	rel;

	if (!OidIsValid(typentry->typrelid))	/* 不应发生 */
		elog(ERROR, "invalid typrelid for composite type %u",
			 typentry->type_id);
	rel = relation_open(typentry->typrelid, AccessShareLock);
	Assert(rel->rd_rel->reltype == typentry->type_id);

	/*
	 * 链接到 tupdesc 并递增其引用计数（我们断言它是
	 * 引用计数型描述符）。此处不用 IncrTupleDescRefCount()，
	 * 因为该引用绝不能登记到当前资源所有者中；它可能
	 * 比当前查询存活得更久。
	 */
	typentry->tupDesc = RelationGetDescr(rel);

	Assert(typentry->tupDesc->tdrefcount > 0);
	typentry->tupDesc->tdrefcount++;

	/*
	 * 将来我们可以费些功夫，在 tupdesc 实际未变时不改
	 * 变 tupDesc_identifier；但目前不值得这么做。
	 */
	typentry->tupDesc_identifier = ++tupledesc_id_counter;

	relation_close(rel, AccessShareLock);
}

/*
 * load_rangetype_info --- 用于设置范围类型信息的辅助例程
 */
static void
load_rangetype_info(TypeCacheEntry *typentry)
{
	Form_pg_range pg_range;
	HeapTuple	tup;
	Oid			subtypeOid;
	Oid			opclassOid;
	Oid			canonicalOid;
	Oid			subdiffOid;
	Oid			opfamilyOid;
	Oid			opcintype;
	Oid			cmpFnOid;

	/* 从 pg_range 获取信息 */
	tup = SearchSysCache1(RANGETYPE, ObjectIdGetDatum(typentry->type_id));
	/* 不应失败，因为我们已检查过 typtype ... */
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for range type %u",
			 typentry->type_id);
	pg_range = (Form_pg_range) GETSTRUCT(tup);

	subtypeOid = pg_range->rngsubtype;
	typentry->rng_collation = pg_range->rngcollation;
	opclassOid = pg_range->rngsubopc;
	canonicalOid = pg_range->rngcanonical;
	subdiffOid = pg_range->rngsubdiff;

	ReleaseSysCache(tup);

	/* 获取算子类属性并查找比较函数 */
	opfamilyOid = get_opclass_family(opclassOid);
	opcintype = get_opclass_input_type(opclassOid);

	cmpFnOid = get_opfamily_proc(opfamilyOid, opcintype, opcintype,
								 BTORDER_PROC);
	if (!RegProcedureIsValid(cmpFnOid))
		elog(ERROR, "missing support function %d(%u,%u) in opfamily %u",
			 BTORDER_PROC, opcintype, opcintype, opfamilyOid);

	/* 设置缓存 fmgrinfo 结构 */
	fmgr_info_cxt(cmpFnOid, &typentry->rng_cmp_proc_finfo,
				  CacheMemoryContext);
	if (OidIsValid(canonicalOid))
		fmgr_info_cxt(canonicalOid, &typentry->rng_canonical_finfo,
					  CacheMemoryContext);
	if (OidIsValid(subdiffOid))
		fmgr_info_cxt(subdiffOid, &typentry->rng_subdiff_finfo,
					  CacheMemoryContext);

	/* 最后建立到元素类型的链接 --- 这标记数据有效 */
	typentry->rngelemtype = lookup_type_cache(subtypeOid, 0);
}

/*
 * load_multirangetype_info --- 设置多范围类型信息
 * 的例程
 */
static void
load_multirangetype_info(TypeCacheEntry *typentry)
{
	Oid			rangetypeOid;

	rangetypeOid = get_multirange_range(typentry->type_id);
	if (!OidIsValid(rangetypeOid))
		elog(ERROR, "cache lookup failed for multirange type %u",
			 typentry->type_id);

	typentry->rngtype = lookup_type_cache(rangetypeOid, TYPECACHE_RANGE_INFO);
}

/*
 * load_domaintype_info --- 用于设置域约束信息的辅助例程
 *
 * 注：我们假定调用发生在生存期较短的环境中，因此扫
 * 描 pg_constraint 期间把数据泄漏到当前环境是可以
 * 的。我们把新的 DomainConstraintCache 数据构建在
 * CurrentMemoryContext 之下，完成时再改挂为
 * 缓存。
 */
static void
load_domaintype_info(TypeCacheEntry *typentry)
{
	Oid			typeOid = typentry->type_id;
	DomainConstraintCache *dcc;
	bool		notNull = false;
	DomainConstraintState **ccons;
	int			cconslen;
	Relation	conRel;
	MemoryContext oldcxt;

	/*
	 * 走到这里说明现有约束信息已过时，因此释放它。
	 * 为安全起见，务必先空置链接再尝试删除数据。
	 */
	if (typentry->domainData)
	{
		dcc = typentry->domainData;
		typentry->domainData = NULL;
		decr_dcc_refcount(dcc);
	}

	/*
	 * 我们尽量优化无域约束这一常见情形，所以在找到约
	 * 束之前不创建 dcc 对象与环境。对临时排序数组也是
	 * 如此。
	 */
	dcc = NULL;
	ccons = NULL;
	cconslen = 0;

	/*
	 * 扫描 pg_constraint 找相关约束。要找的不只是
	 * 本域的约束，还有任何祖先域的约束，因此外层循环
	 * 会沿域栈向上爬升。
	 */
	conRel = table_open(ConstraintRelationId, AccessShareLock);

	for (;;)
	{
		HeapTuple	tup;
		HeapTuple	conTup;
		Form_pg_type typTup;
		int			nccons = 0;
		ScanKeyData key[1];
		SysScanDesc scan;

		tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeOid));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for type %u", typeOid);
		typTup = (Form_pg_type) GETSTRUCT(tup);

		if (typTup->typtype != TYPTYPE_DOMAIN)
		{
			/* 不是域，结束 */
			ReleaseSysCache(tup);
			break;
		}

		/* 测试 NOT NULL 约束 */
		if (typTup->typnotnull)
			notNull = true;

		/* 查找本域上的 CHECK 约束 */
		ScanKeyInit(&key[0],
					Anum_pg_constraint_contypid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(typeOid));

		scan = systable_beginscan(conRel, ConstraintTypidIndexId, true,
								  NULL, 1, key);

		while (HeapTupleIsValid(conTup = systable_getnext(scan)))
		{
			Form_pg_constraint c = (Form_pg_constraint) GETSTRUCT(conTup);
			Datum		val;
			bool		isNull;
			char	   *constring;
			Expr	   *check_expr;
			DomainConstraintState *r;

			/* 忽略非 CHECK 约束（目前不应存在） */
			if (c->contype != CONSTRAINT_CHECK)
				continue;

			/* 预期 conbin 不会为 NULL，但我们仍会测试 */
			val = fastgetattr(conTup, Anum_pg_constraint_conbin,
							  conRel->rd_att, &isNull);
			if (isNull)
				elog(ERROR, "domain \"%s\" constraint \"%s\" has NULL conbin",
					 NameStr(typTup->typname), NameStr(c->conname));

			/* conbin 转成调用者环境内的 C 串 */
			constring = TextDatumGetCString(val);

			/* 按需创建 DomainConstraintCache 对象与环境 */
			if (dcc == NULL)
			{
				MemoryContext cxt;

				cxt = AllocSetContextCreate(CurrentMemoryContext,
											"Domain constraints",
											ALLOCSET_SMALL_SIZES);
				dcc = (DomainConstraintCache *)
					MemoryContextAlloc(cxt, sizeof(DomainConstraintCache));
				dcc->constraints = NIL;
				dcc->dccContext = cxt;
				dcc->dccRefCount = 0;
			}

			/* 在 DomainConstraintCache 环境中创建节点树 */
			oldcxt = MemoryContextSwitchTo(dcc->dccContext);

			check_expr = (Expr *) stringToNode(constring);

			/*
			 * 规划表达式，因为 ExecInitExpr 会要求这样。
			 *
			 * 注：缓存 expression_planner() 的结果并不
			 * 是什么好做法。理想情况下我们在此用
			 * CachedExpression，从而能及时响应内联函数
			 * 变更等情形。但由于我们不支持可变的域
			 * CHECK 约束，所以这样做是否值得付出额外
			 * 开销并不清楚。
			 */
			check_expr = expression_planner(check_expr);

			r = makeNode(DomainConstraintState);
			r->constrainttype = DOM_CONSTRAINT_CHECK;
			r->name = pstrdup(NameStr(c->conname));
			r->check_expr = check_expr;
			r->check_exprstate = NULL;

			MemoryContextSwitchTo(oldcxt);

			/* 把约束累积到数组中，供下方排序 */
			if (ccons == NULL)
			{
				cconslen = 8;
				ccons = (DomainConstraintState **)
					palloc(cconslen * sizeof(DomainConstraintState *));
			}
			else if (nccons >= cconslen)
			{
				cconslen *= 2;
				ccons = (DomainConstraintState **)
					repalloc(ccons, cconslen * sizeof(DomainConstraintState *));
			}
			ccons[nccons++] = r;
		}

		systable_endscan(scan);

		if (nccons > 0)
		{
			/*
			 * 对该域的条目排序，使各项 CHECK 按确定的顺
			 * 序应用。
			 */
			if (nccons > 1)
				qsort(ccons, nccons, sizeof(DomainConstraintState *), dcs_cmp);

			/*
			 * 现在把它们挂上总列表。这里用 lcons() 是因为
			 * 父域的约束应当更早应用。
			 */
			oldcxt = MemoryContextSwitchTo(dcc->dccContext);
			while (nccons > 0)
				dcc->constraints = lcons(ccons[--nccons], dcc->constraints);
			MemoryContextSwitchTo(oldcxt);
		}

		/* 循环到栈中下一个域 */
		typeOid = typTup->typbasetype;
		ReleaseSysCache(tup);
	}

	table_close(conRel, AccessShareLock);

	/*
	 * 无论栈中有多少个域请求，都只需添加一个 NOT NULL
	 * 检查。
	 */
	if (notNull)
	{
		DomainConstraintState *r;

		/* 按需创建 DomainConstraintCache 对象与环境 */
		if (dcc == NULL)
		{
			MemoryContext cxt;

			cxt = AllocSetContextCreate(CurrentMemoryContext,
										"Domain constraints",
										ALLOCSET_SMALL_SIZES);
			dcc = (DomainConstraintCache *)
				MemoryContextAlloc(cxt, sizeof(DomainConstraintCache));
			dcc->constraints = NIL;
			dcc->dccContext = cxt;
			dcc->dccRefCount = 0;
		}

		/* 在 DomainConstraintCache 环境中创建节点树 */
		oldcxt = MemoryContextSwitchTo(dcc->dccContext);

		r = makeNode(DomainConstraintState);

		r->constrainttype = DOM_CONSTRAINT_NOTNULL;
		r->name = pstrdup("NOT NULL");
		r->check_expr = NULL;
		r->check_exprstate = NULL;

		/* 用 lcons 让空值检查最先应用 */
		dcc->constraints = lcons(r, dcc->constraints);

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * 如果创建了约束对象，就把它移入 CacheMemoryContext
	 * 并挂到 typcache 条目上。
	 */
	if (dcc)
	{
		MemoryContextSetParent(dcc->dccContext, CacheMemoryContext);
		typentry->domainData = dcc;
		dcc->dccRefCount++;		/* 统计 typcache 的引用 */
	}

	/* 无论如何，typcache 条目的域数据现在有效。 */
	typentry->flags |= TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS;
}

/*
 * 按名称排序 DomainConstraintState 指针的 qsort 函数
 */
static int
dcs_cmp(const void *a, const void *b)
{
	const DomainConstraintState *const *ca = (const DomainConstraintState *const *) a;
	const DomainConstraintState *const *cb = (const DomainConstraintState *const *) b;

	return strcmp((*ca)->name, (*cb)->name);
}

/*
 * decr_dcc_refcount --- 递减 DomainConstraintCache 的
 * 引用计数，无引用则释放
 */
static void
decr_dcc_refcount(DomainConstraintCache *dcc)
{
	Assert(dcc->dccRefCount > 0);
	if (--(dcc->dccRefCount) <= 0)
		MemoryContextDelete(dcc->dccContext);
}

/*
 * DomainConstraintRef 的环境重置/删除回调
 */
static void
dccref_deletion_callback(void *arg)
{
	DomainConstraintRef *ref = (DomainConstraintRef *) arg;
	DomainConstraintCache *dcc = ref->dcc;

	/* 谨慎起见 --- 务必在尝试释放前先空置链接 */
	if (dcc)
	{
		ref->constraints = NIL;
		ref->dcc = NULL;
		decr_dcc_refcount(dcc);
	}
}

/*
 * prep_domain_constraints --- 准备域约束以供执行
 *
 * DomainConstraintCache 列表中存储的表达式树会被
 * 转换成可执行表达式状态树，存于 execctx 中。
 */
static List *
prep_domain_constraints(List *constraints, MemoryContext execctx)
{
	List	   *result = NIL;
	MemoryContext oldcxt;
	ListCell   *lc;

	oldcxt = MemoryContextSwitchTo(execctx);

	foreach(lc, constraints)
	{
		DomainConstraintState *r = (DomainConstraintState *) lfirst(lc);
		DomainConstraintState *newr;

		newr = makeNode(DomainConstraintState);
		newr->constrainttype = r->constrainttype;
		newr->name = r->name;
		newr->check_expr = r->check_expr;
		newr->check_exprstate = ExecInitExpr(r->check_expr, NULL);

		result = lappend(result, newr);
	}

	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * InitDomainConstraintRef --- 初始化 DomainConstraintRef
 *
 * 调用者必须告知我们 DomainConstraintRef 所处的
 * 内存上下文。该环境重置/删除时，ref 会被清理。
 *
 * 调用者还必须告知我们，是否希望在挂接
 * 本 ref 的 DomainConstraintState 节点中计算
 * check_exprstate 字段。若不希望则无需复制列表。
 */
void
InitDomainConstraintRef(Oid type_id, DomainConstraintRef *ref,
						MemoryContext refctx, bool need_exprstate)
{
	/* 查找 typcache 条目 --- 我们假定它永久存活 */
	ref->tcache = lookup_type_cache(type_id, TYPECACHE_DOMAIN_CONSTR_INFO);
	ref->need_exprstate = need_exprstate;
	/* 为安全起见，先建立回调再获取引用计数 */
	ref->refctx = refctx;
	ref->dcc = NULL;
	ref->callback.func = dccref_deletion_callback;
	ref->callback.arg = (void *) ref;
	MemoryContextRegisterResetCallback(refctx, &ref->callback);
	/* 若有约束则获取引用计数，并建立导出的列表 */
	if (ref->tcache->domainData)
	{
		ref->dcc = ref->tcache->domainData;
		ref->dcc->dccRefCount++;
		if (ref->need_exprstate)
			ref->constraints = prep_domain_constraints(ref->dcc->constraints,
													   ref->refctx);
		else
			ref->constraints = ref->dcc->constraints;
	}
	else
		ref->constraints = NIL;
}

/*
 * UpdateDomainConstraintRef --- 复核域约束信息的有效性
 *
 * 如果域的约束集变了，ref->constraints 会被更新为指
 * 向新的缓存约束列表。
 *
 * 在域未发生任何变化的正常情形下，这个检查足够廉
 * 价，在 *每次* 使用约束信息前都检查是合理的（
 * 也是预期的）。
 */
void
UpdateDomainConstraintRef(DomainConstraintRef *ref)
{
	TypeCacheEntry *typentry = ref->tcache;

	/* 确保 typcache 条目的数据是最新的 */
	if ((typentry->flags & TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS) == 0 &&
		typentry->typtype == TYPTYPE_DOMAIN)
		load_domaintype_info(typentry);

	/* 若有新信息则转移给 ref 对象，并调整引用计数 */
	if (ref->dcc != typentry->domainData)
	{
		/* 谨慎起见 --- 务必在尝试释放前先空置链接 */
		DomainConstraintCache *dcc = ref->dcc;

		if (dcc)
		{
			/*
			 * 注：我们只是泄漏掉先前的可执行域约束
			 * 列表。或者我们可以把它们保存在
			 * ref->refctx 的子环境，并在此刻释放该环境。
			 * 然而实践中此路径极少走到，为子环境额外
			 * 做簿记似乎不值得；就允许在 refctx 生存期
			 * 内泄漏。
			 */
			ref->constraints = NIL;
			ref->dcc = NULL;
			decr_dcc_refcount(dcc);
		}
		dcc = typentry->domainData;
		if (dcc)
		{
			ref->dcc = dcc;
			dcc->dccRefCount++;
			if (ref->need_exprstate)
				ref->constraints = prep_domain_constraints(dcc->constraints,
														   ref->refctx);
			else
				ref->constraints = dcc->constraints;
		}
	}
}

/*
 * DomainHasConstraints --- 检查域是否带有约束的实用例程
 *
 * 定义为：若类型不是域则返回假，而非报错。
 */
bool
DomainHasConstraints(Oid type_id)
{
	TypeCacheEntry *typentry;

	/*
	 * 注：一个副作用是使 typcache 的域数据变为
	 * 有效。这没问题，若真有域约束很快就要用它。
	 */
	typentry = lookup_type_cache(type_id, TYPECACHE_DOMAIN_CONSTR_INFO);

	return (typentry->domainData != NULL);
}


/*
 * array_element_has_equality 及其伙伴例程是用于检查
 * 我们是否应当相信 array_eq 及相关函数能用于给定
 * 的数组类型或复合类型的。
 *
 * 上面的逻辑可能在同一个类型条目上反复调用它们，
 * 所以一旦得知结果就用 typentry->flags 字段缓存。
 * 另外，我们假定只要想要其中任一事实，就很可
 * 能想要该类型的全部事实，故只查找组件数据类
 * 型做一次查找。
 */

static bool
array_element_has_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EQUALITY) != 0;
}

static bool
array_element_has_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_COMPARE) != 0;
}

static bool
array_element_has_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_HASHING) != 0;
}

static bool
array_element_has_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_array_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EXTENDED_HASHING) != 0;
}

static void
cache_array_element_properties(TypeCacheEntry *typentry)
{
	Oid			elem_type = get_base_element_type(typentry->type_id);

	if (OidIsValid(elem_type))
	{
		TypeCacheEntry *elementry;

		elementry = lookup_type_cache(elem_type,
									  TYPECACHE_EQ_OPR |
									  TYPECACHE_CMP_PROC |
									  TYPECACHE_HASH_PROC |
									  TYPECACHE_HASH_EXTENDED_PROC);
		if (OidIsValid(elementry->eq_opr))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EQUALITY;
		if (OidIsValid(elementry->cmp_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_COMPARE;
		if (OidIsValid(elementry->hash_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_HASHING;
		if (OidIsValid(elementry->hash_extended_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EXTENDED_HASHING;
	}
	typentry->flags |= TCFLAGS_CHECKED_ELEM_PROPERTIES;
}

/*
 * 同样也有用于复合类型的辅助函数。
 */

static bool
record_fields_have_equality(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_EQUALITY) != 0;
}

static bool
record_fields_have_compare(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_COMPARE) != 0;
}

static bool
record_fields_have_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_HASHING) != 0;
}

static bool
record_fields_have_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_FIELD_PROPERTIES))
		cache_record_field_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_FIELD_EXTENDED_HASHING) != 0;
}

static void
cache_record_field_properties(TypeCacheEntry *typentry)
{
	/*
	 * 对于 RECORD 类型，我们无法真正判断什么可用，因
	 * 为在此处拿不到具体的匿名类型。就只假定相等与
	 * 比较可用（运行时可能失败）。我们也可宣称哈
	 * 希可用，但如果代码在基于比较（排序）与基于
	 * 哈希的计划之间有权选择时选了哈希，那么本可
	 * 成功的东西就可能失败。实践中，支持比较的类
	 * 型要多于支持
	 * 哈希。
	 */
	if (typentry->type_id == RECORDOID)
	{
		typentry->flags |= (TCFLAGS_HAVE_FIELD_EQUALITY |
							TCFLAGS_HAVE_FIELD_COMPARE);
	}
	else if (typentry->typtype == TYPTYPE_COMPOSITE)
	{
		TupleDesc	tupdesc;
		int			newflags;
		int			i;

		/* 若还没有则取复合类型的 tupdesc */
		if (typentry->tupDesc == NULL)
			load_typcache_tupdesc(typentry);
		tupdesc = typentry->tupDesc;

		/* 做额外目录查找期间必须递增引用计数 */
		IncrTupleDescRefCount(tupdesc);

		/* 若全部非删除字段都有该属性，则具有之 */
		newflags = (TCFLAGS_HAVE_FIELD_EQUALITY |
					TCFLAGS_HAVE_FIELD_COMPARE |
					TCFLAGS_HAVE_FIELD_HASHING |
					TCFLAGS_HAVE_FIELD_EXTENDED_HASHING);
		for (i = 0; i < tupdesc->natts; i++)
		{
			TypeCacheEntry *fieldentry;
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;

			fieldentry = lookup_type_cache(attr->atttypid,
										   TYPECACHE_EQ_OPR |
										   TYPECACHE_CMP_PROC |
										   TYPECACHE_HASH_PROC |
										   TYPECACHE_HASH_EXTENDED_PROC);
			if (!OidIsValid(fieldentry->eq_opr))
				newflags &= ~TCFLAGS_HAVE_FIELD_EQUALITY;
			if (!OidIsValid(fieldentry->cmp_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_COMPARE;
			if (!OidIsValid(fieldentry->hash_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_HASHING;
			if (!OidIsValid(fieldentry->hash_extended_proc))
				newflags &= ~TCFLAGS_HAVE_FIELD_EXTENDED_HASHING;

			/* 一旦证伪所有比特就可以跳出循环 */
			if (newflags == 0)
				break;
		}
		typentry->flags |= newflags;

		DecrTupleDescRefCount(tupdesc);
	}
	else if (typentry->typtype == TYPTYPE_DOMAIN)
	{
		/* 若是复合类型的域，复制基础类型属性 */
		TypeCacheEntry *baseentry;

		/* 若尚未加载则加载基础类型信息 */
		if (typentry->domainBaseType == InvalidOid)
		{
			typentry->domainBaseTypmod = -1;
			typentry->domainBaseType =
				getBaseTypeAndTypmod(typentry->type_id,
									 &typentry->domainBaseTypmod);
		}
		baseentry = lookup_type_cache(typentry->domainBaseType,
									  TYPECACHE_EQ_OPR |
									  TYPECACHE_CMP_PROC |
									  TYPECACHE_HASH_PROC |
									  TYPECACHE_HASH_EXTENDED_PROC);
		if (baseentry->typtype == TYPTYPE_COMPOSITE)
		{
			typentry->flags |= TCFLAGS_DOMAIN_BASE_IS_COMPOSITE;
			typentry->flags |= baseentry->flags & (TCFLAGS_HAVE_FIELD_EQUALITY |
												   TCFLAGS_HAVE_FIELD_COMPARE |
												   TCFLAGS_HAVE_FIELD_HASHING |
												   TCFLAGS_HAVE_FIELD_EXTENDED_HASHING);
		}
	}
	typentry->flags |= TCFLAGS_CHECKED_FIELD_PROPERTIES;
}

/*
 * 同样有用于范围和多范围类型的辅助函数。
 *
 * 我们可以借用数组元素属性的标志位，把它们用于范
 * 围元素属性，因为这些标志位在范围或多范围类
 * 型 typcache 条目中无其他用途。
 */

static bool
range_element_has_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_range_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_HASHING) != 0;
}

static bool
range_element_has_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_range_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EXTENDED_HASHING) != 0;
}

static void
cache_range_element_properties(TypeCacheEntry *typentry)
{
	/* 若尚未加载则加载子类型链接 */
	if (typentry->rngelemtype == NULL &&
		typentry->typtype == TYPTYPE_RANGE)
		load_rangetype_info(typentry);

	if (typentry->rngelemtype != NULL)
	{
		TypeCacheEntry *elementry;

		/* 可能需要计算子类型的哈希函数属性 */
		elementry = lookup_type_cache(typentry->rngelemtype->type_id,
									  TYPECACHE_HASH_PROC |
									  TYPECACHE_HASH_EXTENDED_PROC);
		if (OidIsValid(elementry->hash_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_HASHING;
		if (OidIsValid(elementry->hash_extended_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EXTENDED_HASHING;
	}
	typentry->flags |= TCFLAGS_CHECKED_ELEM_PROPERTIES;
}

static bool
multirange_element_has_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_multirange_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_HASHING) != 0;
}

static bool
multirange_element_has_extended_hashing(TypeCacheEntry *typentry)
{
	if (!(typentry->flags & TCFLAGS_CHECKED_ELEM_PROPERTIES))
		cache_multirange_element_properties(typentry);
	return (typentry->flags & TCFLAGS_HAVE_ELEM_EXTENDED_HASHING) != 0;
}

static void
cache_multirange_element_properties(TypeCacheEntry *typentry)
{
	/* 若尚未加载则加载范围链接 */
	if (typentry->rngtype == NULL &&
		typentry->typtype == TYPTYPE_MULTIRANGE)
		load_multirangetype_info(typentry);

	if (typentry->rngtype != NULL && typentry->rngtype->rngelemtype != NULL)
	{
		TypeCacheEntry *elementry;

		/* 可能需要计算子类型的哈希函数属性 */
		elementry = lookup_type_cache(typentry->rngtype->rngelemtype->type_id,
									  TYPECACHE_HASH_PROC |
									  TYPECACHE_HASH_EXTENDED_PROC);
		if (OidIsValid(elementry->hash_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_HASHING;
		if (OidIsValid(elementry->hash_extended_proc))
			typentry->flags |= TCFLAGS_HAVE_ELEM_EXTENDED_HASHING;
	}
	typentry->flags |= TCFLAGS_CHECKED_ELEM_PROPERTIES;
}

/*
 * 确保 RecordCacheArray 和 RecordIdentifierArray 足够大
 * 以存 'typmod'。
 */
static void
ensure_record_cache_typmod_slot_exists(int32 typmod)
{
	if (RecordCacheArray == NULL)
	{
		RecordCacheArray = (RecordCacheArrayEntry *)
			MemoryContextAllocZero(CacheMemoryContext,
								   64 * sizeof(RecordCacheArrayEntry));
		RecordCacheArrayLen = 64;
	}

	if (typmod >= RecordCacheArrayLen)
	{
		int32		newlen = pg_nextpower2_32(typmod + 1);

		RecordCacheArray = repalloc0_array(RecordCacheArray,
										   RecordCacheArrayEntry,
										   RecordCacheArrayLen,
										   newlen);
		RecordCacheArrayLen = newlen;
	}
}

/*
 * lookup_rowtype_tupdesc_internal --- 查找行类型的内部例程
 *
 * 与 lookup_rowtype_tupdesc_noerror API 相同，但返回的
 * tupdesc 未加引用计数。
 */
static TupleDesc
lookup_rowtype_tupdesc_internal(Oid type_id, int32 typmod, bool noError)
{
	if (type_id != RECORDOID)
	{
		/*
		 * 这是命名的复合类型，所以用常规 typcache。
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL && !noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * 这是瞬态记录类型，所以在记录类型表中找。
		 */
		if (typmod >= 0)
		{
			/* 已在我们本地缓存中？ */
			if (typmod < RecordCacheArrayLen &&
				RecordCacheArray[typmod].tupdesc != NULL)
				return RecordCacheArray[typmod].tupdesc;

			/* 是否已挂接共享记录 typmod 注册表？ */
			if (CurrentSession->shared_typmod_registry != NULL)
			{
				SharedTypmodTableEntry *entry;

				/* 尝试在共享 typmod 索引找到它 */
				entry = dshash_find(CurrentSession->shared_typmod_table,
									&typmod, false);
				if (entry != NULL)
				{
					TupleDesc	tupdesc;

					tupdesc = (TupleDesc)
						dsa_get_address(CurrentSession->area,
										entry->shared_tupdesc);
					Assert(typmod == tupdesc->tdtypmod);

					/* 我们可能要扩展本地 RecordCacheArray */
					ensure_record_cache_typmod_slot_exists(typmod);

					/*
					 * 本地数组现在能直接指向共享内存中的
					 * 那个 TupleDesc，其无引用计数。
					 */
					RecordCacheArray[typmod].tupdesc = tupdesc;
					Assert(tupdesc->tdrefcount == -1);

					/*
					 * tupdesc 标识符不跨进程共享，故本地分配
					 * 一个。
					 */
					RecordCacheArray[typmod].id = ++tupledesc_id_counter;

					dshash_release_lock(CurrentSession->shared_typmod_table,
										entry);

					return RecordCacheArray[typmod].tupdesc;
				}
			}
		}

		if (!noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("record type has not been registered")));
		return NULL;
	}
}

/*
 * lookup_rowtype_tupdesc
 *
 * 给定应描述某个已知复合类型的 typeid/typmod，返回
 * 该类型的元组描述符。失败时会 ereport。（用
 * ereport 是因为本函数可被用户指定的 OID 到达，
 * 例如来自 record_in()。）
 *
 * 注：成功时我们会递增返回 TupleDesc 的引用计数，
 * 并在 CurrentResourceOwner 中登记该引用。调用者用完
 * tupdesc 后必须调用 ReleaseTupleDesc。（有些情形
 * 下返回的 tupdesc 不带引用计数，此时
 * PinTupleDesc/ReleaseTupleDesc 为空操作；但此时
 * tupdesc 保证存活到进程退出为止。）
 */
TupleDesc
lookup_rowtype_tupdesc(Oid type_id, int32 typmod)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	PinTupleDesc(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_noerror
 *
 * 同上，但如果类型不是已知复合类型且 noError 为
 * 真，则返回 NULL 而不是 ereport。（注意若传入的
 * type_id 是假的，也照样 ereport。）
 */
TupleDesc
lookup_rowtype_tupdesc_noerror(Oid type_id, int32 typmod, bool noError)
{
	TupleDesc	tupDesc;

	tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, noError);
	if (tupDesc != NULL)
		PinTupleDesc(tupDesc);
	return tupDesc;
}

/*
 * lookup_rowtype_tupdesc_copy
 *
 * 与 lookup_rowtype_tupdesc() 类似，但返回的 TupleDesc
 * 已被复制到 CurrentMemoryContext 中且不带引用计数。
 */
TupleDesc
lookup_rowtype_tupdesc_copy(Oid type_id, int32 typmod)
{
	TupleDesc	tmp;

	tmp = lookup_rowtype_tupdesc_internal(type_id, typmod, false);
	return CreateTupleDescCopyConstr(tmp);
}

/*
 * lookup_rowtype_tupdesc_domain
 *
 * 与 lookup_rowtype_tupdesc_noerror() 相同，区别在于类型还可以
 * 是命名复合类型上的域；因此本函数实际上等同于
 * lookup_rowtype_tupdesc_noerror(getBaseType(type_id), typmod, noError)
 * 只是稍快一点。
 *
 * 注：我们不把穿透域的查找行为并入普通
 * lookup_rowtype_tupdesc() 的原因是要让调用者知道
 * 自己可能在和域打交道。否则他们可能构造出一个本应
 * 属于域类型的元组，却不施加域约束。
 */
TupleDesc
lookup_rowtype_tupdesc_domain(Oid type_id, int32 typmod, bool noError)
{
	TupleDesc	tupDesc;

	if (type_id != RECORDOID)
	{
		/*
		 * 检查是域还是命名复合类型，顺便加载所需
		 * 的数据。
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id,
									 TYPECACHE_TUPDESC |
									 TYPECACHE_DOMAIN_BASE_INFO);
		if (typentry->typtype == TYPTYPE_DOMAIN)
			return lookup_rowtype_tupdesc_noerror(typentry->domainBaseType,
												  typentry->domainBaseTypmod,
												  noError);
		if (typentry->tupDesc == NULL && !noError)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		tupDesc = typentry->tupDesc;
	}
	else
		tupDesc = lookup_rowtype_tupdesc_internal(type_id, typmod, noError);
	if (tupDesc != NULL)
		PinTupleDesc(tupDesc);
	return tupDesc;
}

/*
 * RecordCacheEntry 哈希表的哈希函数。
 */
static uint32
record_type_typmod_hash(const void *data, size_t size)
{
	RecordCacheEntry *entry = (RecordCacheEntry *) data;

	return hashTupleDesc(entry->tupdesc);
}

/*
 * RecordCacheEntry 哈希表的匹配函数。
 */
static int
record_type_typmod_compare(const void *a, const void *b, size_t size)
{
	RecordCacheEntry *left = (RecordCacheEntry *) a;
	RecordCacheEntry *right = (RecordCacheEntry *) b;

	return equalTupleDescs(left->tupdesc, right->tupdesc) ? 0 : 1;
}

/*
 * assign_record_type_typmod
 *
 * 给定 RECORD 类型的元组描述符，为该类型查找或创建
 * 缓存条目，并把 tupdesc 的 tdtypmod 字段设为一个能让
 * lookup_rowtype_tupdesc 认出的值。
 */
void
assign_record_type_typmod(TupleDesc tupDesc)
{
	RecordCacheEntry *recentry;
	TupleDesc	entDesc;
	bool		found;
	MemoryContext oldcxt;

	Assert(tupDesc->tdtypeid == RECORDOID);

	if (RecordCacheHash == NULL)
	{
		/* 首次进入：初始化哈希表 */
		HASHCTL		ctl;

		ctl.keysize = sizeof(TupleDesc);	/* 仅指针 */
		ctl.entrysize = sizeof(RecordCacheEntry);
		ctl.hash = record_type_typmod_hash;
		ctl.match = record_type_typmod_compare;
		RecordCacheHash = hash_create("Record information cache", 64,
									  &ctl,
									  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

		/* 同时确保 CacheMemoryContext 存在 */
		if (!CacheMemoryContext)
			CreateCacheMemoryContext();
	}

	/*
	 * 为该元组描述符找哈希表条目。我们暂不使用
	 * HASH_ENTER，因为如果它缺失，就要确保所有分配都
	 * 在我们创建新条目之前成功。
	 */
	recentry = (RecordCacheEntry *) hash_search(RecordCacheHash,
												&tupDesc,
												HASH_FIND, &found);
	if (found && recentry->tupdesc != NULL)
	{
		tupDesc->tdtypmod = recentry->tupdesc->tdtypmod;
		return;
	}

	/* 不存在，所以需要制造一个条目 */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	/* 若已挂接，在 SharedRecordTypmodRegistry 找 */
	entDesc = find_or_make_matching_shared_tupledesc(tupDesc);
	if (entDesc == NULL)
	{
		/*
		 * 先确保有空间，再去 CreateTupleDescCopy() 或推进
		 * NextRecordTypmod
		 */
		ensure_record_cache_typmod_slot_exists(NextRecordTypmod);

		/* 仅引用计数的本地缓存。 */
		entDesc = CreateTupleDescCopy(tupDesc);
		entDesc->tdrefcount = 1;
		entDesc->tdtypmod = NextRecordTypmod++;
	}
	else
	{
		ensure_record_cache_typmod_slot_exists(entDesc->tdtypmod);
	}

	RecordCacheArray[entDesc->tdtypmod].tupdesc = entDesc;

	/* 也分配唯一的 tupdesc 标识符。 */
	RecordCacheArray[entDesc->tdtypmod].id = ++tupledesc_id_counter;

	/* 全部初始化完毕；创建哈希表条目 */
	recentry = (RecordCacheEntry *) hash_search(RecordCacheHash,
												&tupDesc,
												HASH_ENTER, NULL);
	recentry->tupdesc = entDesc;

	/* 更新调用者的元组描述符。 */
	tupDesc->tdtypmod = entDesc->tdtypmod;

	MemoryContextSwitchTo(oldcxt);
}

/*
 * assign_record_type_identifier
 *
 * 获取一个标识符，它在后端进程的整个生存期内唯一，
 * 用于指定复合类型当前的元组描述符。对命名的复合类
 * 型来说，若类型定义改变，该值保证会改变。对已注册
 * 的 RECORD 类型，该值一经分配就不会再变，因为已注
 * 册的类型也不会变。若指定的是匿名 RECORD 类型，我
 * 们每次调用都返回一个新标识符。
 */
uint64
assign_record_type_identifier(Oid type_id, int32 typmod)
{
	if (type_id != RECORDOID)
	{
		/*
		 * 这是命名的复合类型，所以用常规 typcache。
		 */
		TypeCacheEntry *typentry;

		typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
		if (typentry->tupDesc == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("type %s is not composite",
							format_type_be(type_id))));
		Assert(typentry->tupDesc_identifier != 0);
		return typentry->tupDesc_identifier;
	}
	else
	{
		/*
		 * 这是瞬态记录类型，所以在记录类型表中找。
		 */
		if (typmod >= 0 && typmod < RecordCacheArrayLen &&
			RecordCacheArray[typmod].tupdesc != NULL)
		{
			Assert(RecordCacheArray[typmod].id != 0);
			return RecordCacheArray[typmod].id;
		}

		/* 对匿名或未识别的记录类型，生成新 ID */
		return ++tupledesc_id_counter;
	}
}

/*
 * 返回容纳一个 SharedRecordTypmodRegistry 所需的共享内存量。
 * 它只是为了不把 SharedRecordTypmodRegistry
 * 的私有细节暴露在头文件中。
 */
size_t
SharedRecordTypmodRegistryEstimate(void)
{
	return sizeof(SharedRecordTypmodRegistry);
}

/*
 * 在预先存在的共享内存区域中初始化 'registry'，该区域
 * 必须最大对齐，并留有 SharedRecordTypmodRegistryEstimate()
 * 字节
 *
 * 'area' 将用于按需分配 typemod 注册所需的共享内
 * 存空间。当前进程预期是并行查询中的 leader 进程，会
 * 被自动挂接，其当前记录类型会载入 *registry。挂接
 * 期间，对 assign_record_type_typmod 的所有调用都将
 * 使用共享注册表。worker 后端
 * 则需要显式挂接。
 *
 * 注意本函数把 'area' 和 'segment' 当作参数传入，而不是
 * 经由 CurrentSession 访问，因为它们要到本函数运行
 * 之后才安装到那里。
 */
void
SharedRecordTypmodRegistryInit(SharedRecordTypmodRegistry *registry,
							   dsm_segment *segment,
							   dsa_area *area)
{
	MemoryContext old_context;
	dshash_table *record_table;
	dshash_table *typmod_table;
	int32		typmod;

	Assert(!IsParallelWorker());

	/* 我们不能已挂接到某个共享注册表。 */
	Assert(CurrentSession->shared_typmod_registry == NULL);
	Assert(CurrentSession->shared_record_table == NULL);
	Assert(CurrentSession->shared_typmod_table == NULL);

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* 创建以元组描述符自身作键的哈希表。 */
	record_table = dshash_create(area, &srtr_record_table_params, area);

	/* 创建以 typmod 索引元组描述符的哈希表。 */
	typmod_table = dshash_create(area, &srtr_typmod_table_params, NULL);

	MemoryContextSwitchTo(old_context);

	/* 初始化 SharedRecordTypmodRegistry。 */
	registry->record_table_handle = dshash_get_hash_table_handle(record_table);
	registry->typmod_table_handle = dshash_get_hash_table_handle(typmod_table);
	pg_atomic_init_u32(&registry->next_typmod, NextRecordTypmod);

	/*
	 * 把本后端私有注册表中的所有条目复制到共享注册
	 * 表中。
	 */
	for (typmod = 0; typmod < NextRecordTypmod; ++typmod)
	{
		SharedTypmodTableEntry *typmod_table_entry;
		SharedRecordTableEntry *record_table_entry;
		SharedRecordTableKey record_table_key;
		dsa_pointer shared_dp;
		TupleDesc	tupdesc;
		bool		found;

		tupdesc = RecordCacheArray[typmod].tupdesc;
		if (tupdesc == NULL)
			continue;

		/* 把 TupleDesc 复制到共享内存。 */
		shared_dp = share_tupledesc(area, tupdesc, typmod);

		/* 插入 typmod 表。 */
		typmod_table_entry = dshash_find_or_insert(typmod_table,
												   &tupdesc->tdtypmod,
												   &found);
		if (found)
			elog(ERROR, "cannot create duplicate shared record typmod");
		typmod_table_entry->typmod = tupdesc->tdtypmod;
		typmod_table_entry->shared_tupdesc = shared_dp;
		dshash_release_lock(typmod_table, typmod_table_entry);

		/* 插入记录表。 */
		record_table_key.shared = false;
		record_table_key.u.local_tupdesc = tupdesc;
		record_table_entry = dshash_find_or_insert(record_table,
												   &record_table_key,
												   &found);
		if (!found)
		{
			record_table_entry->key.shared = true;
			record_table_entry->key.u.shared_tupdesc = shared_dp;
		}
		dshash_release_lock(record_table, record_table_entry);
	}

	/*
	 * 建立全局状态，让 assign_record_type_typmod 与
	 * lookup_rowtype_tupdesc_internal 知晓共享注册表。
	 */
	CurrentSession->shared_record_table = record_table;
	CurrentSession->shared_typmod_table = typmod_table;
	CurrentSession->shared_typmod_registry = registry;

	/*
	 * 我们在 leader 中安装分离钩子，但只是为了处理
	 * GetSessionDsmHandle() 期间的失败。一旦把内存
	 * 固定后，leader 进程就会一直用共享注册表直至退
	 * 出。
	 */
	on_dsm_detach(segment, shared_record_typmod_registry_detach, (Datum) 0);
}

/*
 * 挂接到 'registry'，它必须已被另一个后端初始化
 * 过。此后对 assign_record_type_typmod 与
 * lookup_rowtype_tupdesc_internal 的调用都会用共享注册
 * 表直至会话分离。
 */
void
SharedRecordTypmodRegistryAttach(SharedRecordTypmodRegistry *registry)
{
	MemoryContext old_context;
	dshash_table *record_table;
	dshash_table *typmod_table;

	Assert(IsParallelWorker());

	/* 我们不能已挂接到某个共享注册表。 */
	Assert(CurrentSession != NULL);
	Assert(CurrentSession->segment != NULL);
	Assert(CurrentSession->area != NULL);
	Assert(CurrentSession->shared_typmod_registry == NULL);
	Assert(CurrentSession->shared_record_table == NULL);
	Assert(CurrentSession->shared_typmod_table == NULL);

	/*
	 * 本地缓存中不能已有 typmod，因为它们会与
	 * SharedRecordTypmodRegistryInit 导入的相冲突。这
	 * 必须是刚启动的并行工作进程。若将来支持
	 * 工作进程回收，worker 就得在服务不同查询之间清
	 * 空其本地缓存，才能调用本函数并与新 leader 同步
	 * typmod；但这有问题，因为我们无法很确定记录
	 * typmod 相关状态没有逃逸到进程中的其他任何地
	 * 方。
	 */
	Assert(NextRecordTypmod == 0);

	old_context = MemoryContextSwitchTo(TopMemoryContext);

	/* 挂接到这两个哈希表。 */
	record_table = dshash_attach(CurrentSession->area,
								 &srtr_record_table_params,
								 registry->record_table_handle,
								 CurrentSession->area);
	typmod_table = dshash_attach(CurrentSession->area,
								 &srtr_typmod_table_params,
								 registry->typmod_table_handle,
								 NULL);

	MemoryContextSwitchTo(old_context);

	/*
	 * 设置工作进程退出时运行的分离钩子。目前它与
	 * leader 的分离钩子相同，但将来二者可能需要不
	 * 同。
	 */
	on_dsm_detach(CurrentSession->segment,
				  shared_record_typmod_registry_detach,
				  PointerGetDatum(registry));

	/*
	 * 建立会话状态，让 assign_record_type_typmod 与
	 * lookup_rowtype_tupdesc_internal 知晓共享注册表。
	 */
	CurrentSession->shared_typmod_registry = registry;
	CurrentSession->shared_record_table = record_table;
	CurrentSession->shared_typmod_table = typmod_table;
}

/*
 * TypeCacheRelCallback
 *		relcache 失效回调函数
 *
 * 删除给定 rel 的复合类型（若有）的缓存元组描述符，
 * 或者当 relid == InvalidOid 时删除所有复合类型的；
 * 同时重置我们已缓存的复合类型可比较性信息。
 *
 * 当给定 relid 发生 relcache 失效事件时调用本函数。
 * 我们必须扫描整个 typcache 哈希，因为不知道该
 * relid 对应的类型 OID。如果这是 pg_type 上的
 * syscache 冲刷回调，就能直接搜索，但那样的话所有
 * 能修改行类型的类 ALTER TABLE 命令都得针对 rel 的
 * pg_type OID 发 syscache 失效。额外的 SI 信号开销很
 * 可能超过我们省下的，因为多数用法中后端 typcache
 * 里的条目并不多。而遗漏型缺陷的风险看来也很
 * 高。
 *
 * 另一种只带局部影响的可能做法，是维护第二个哈
 * 希表，按 typrelid 索引复合类型的 typcache 条目。
 * 但它是否值得麻烦仍不明确。
 */
static void
TypeCacheRelCallback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash 必须存在，否则本回调不会被注册 */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (typentry->typtype == TYPTYPE_COMPOSITE)
		{
			/* 若无匹配则跳过，除非在清所有复合类型 */
			if (relid != typentry->typrelid && relid != InvalidOid)
				continue;

			/* 若有 tupdesc 则删除它 */
			if (typentry->tupDesc != NULL)
			{
				/*
				 * 释放引用计数，若无剩余则释放 tupdesc。
				 * （不能用 DecrTupleDescRefCount，因为此引用
				 * 并未登记到资源所有者。）
				 */
				Assert(typentry->tupDesc->tdrefcount > 0);
				if (--typentry->tupDesc->tdrefcount == 0)
					FreeTupleDesc(typentry->tupDesc);
				typentry->tupDesc = NULL;

				/*
				 * 同时清空 tupDesc_identifier，让任何关注它的
				 * 东西都能意识到 tupdesc 可能已变更。
				 * （或者，本可规定：要探测可能的 tupdesc
				 * 变更，必须既检查 tupDesc != NULL，又检查
				 * tupDesc_identifier 与先前所见相同。那样似乎
				 * 容易出错。）
				 */
				typentry->tupDesc_identifier = 0;
			}

			/* 重置相等/比较/哈希有效性信息 */
			typentry->flags &= ~TCFLAGS_OPERATOR_FLAGS;
		}
		else if (typentry->typtype == TYPTYPE_DOMAIN)
		{
			/*
			 * 如果是复合类型上的域，就重置标志。（我们
			 * 不去判断具体基础类型是否要重置。）注意
			 * 如果尚未判定基础类型是否为复合，那就无
			 * 需重置任何东西。
			 */
			if (typentry->flags & TCFLAGS_DOMAIN_BASE_IS_COMPOSITE)
				typentry->flags &= ~TCFLAGS_OPERATOR_FLAGS;
		}
	}
}

/*
 * TypeCacheTypCallback
 *		syscache 失效回调函数
 *
 * 当任意 pg_type 行发生 syscache 失效事件时调用本
 * 函数。若我们已缓存该类型的信息，就标记
 * 其需要重新加载。
 */
static void
TypeCacheTypCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash 必须存在，否则本回调不会被注册 */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		/* 这是目标类型行吗（还是整缓存冲刷）？ */
		if (hashvalue == 0 || typentry->type_id_hash == hashvalue)
		{
			/*
			 * 把直接得自 pg_type 的数据标记为无效。另外，如
			 * 果是域，typnotnull 可能已变，所以需要重新计算
			 * 其约束。
			 */
			typentry->flags &= ~(TCFLAGS_HAVE_PG_TYPE_DATA |
								 TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS);
		}
	}
}

/*
 * TypeCacheOpcCallback
 *		syscache 失效回调函数
 *
 * 当任意 pg_opclass 行发生 syscache 失效事件时调用
 * 本函数。原则上我们大概率可以只使依赖特定算子类
 * 的数据失效，但由于生产环境里 pg_opclass 的更新很
 * 罕见，为此搞复杂不值：我们只把全部缓存标记为
 * 无效
 *
 * 注意我们并不费心关注 pg_amop 或 pg_amproc 的更新。
 * 这应当是安全的，因为 ALTER OPERATOR FAMILY ADD/DROP
 * OPERATOR/FUNCTION 不允许用来增删算子类的主算子和
 * 主函数，只允许增删算子族的跨类型成员；而后者这
 * 类成员不会被缓存到这里。
 */
static void
TypeCacheOpcCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	TypeCacheEntry *typentry;

	/* TypeCacheHash 必须存在，否则本回调不会被注册 */
	hash_seq_init(&status, TypeCacheHash);
	while ((typentry = (TypeCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		/* 重置相等/比较/哈希有效性信息 */
		typentry->flags &= ~TCFLAGS_OPERATOR_FLAGS;
	}
}

/*
 * TypeCacheConstrCallback
 *		syscache 失效回调函数
 *
 * 当任意 pg_constraint 行发生 syscache 失效事件时调用
 * 本函数。这种事情发生时，我们会冲刷域约束
 * 的信息。
 *
 * 有点恼人的是，我们无法分辨失效事件是否针对域
 * 约束记录；通常表约束的更新流量比域约束多得多，
 * 所以我们会做大量无用的冲刷。不过这仍胜过
 * 以前那种对域约束完全不缓存的
 * 做法。
 */
static void
TypeCacheConstrCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	TypeCacheEntry *typentry;

	/*
	 * 由于本函数调用得非常频繁，而 typcache 条目中通常
	 * 只有很少一部分是域，所以这里不用 hash_seq_search。
	 * 而是把所有域类型条目串成一条链，以便能廉价地
	 * 访问它们。
	 */
	for (typentry = firstDomainTypeEntry;
		 typentry != NULL;
		 typentry = typentry->nextDomain)
	{
		/* 重置域约束有效性信息 */
		typentry->flags &= ~TCFLAGS_CHECKED_DOMAIN_CONSTRAINTS;
	}
}


/*
 * 检查给定 OID 是否属于可按比较排序的子集
 */
static inline bool
enum_known_sorted(TypeCacheEnumData *enumdata, Oid arg)
{
	Oid			offset;

	if (arg < enumdata->bitmap_base)
		return false;
	offset = arg - enumdata->bitmap_base;
	if (offset > (Oid) INT_MAX)
		return false;
	return bms_is_member((int) offset, enumdata->sorted_values);
}


/*
 * compare_values_of_enum
 *		比较枚举类型的两个成员。
 *		按 arg1 <、= 或 > arg2 分别返回 <0/0/>0。
 *
 * 注：目前，只有当被要求比较一个尚不在缓存中的
 * 枚举值时才会刷新 enumData 缓存。这没问题，因为
 * 不支持重排现有值，因此即使自我们上次加载缓存
 * 之后又添加了其他值，先前缓存值的比较也会返回正
 * 确答案。
 *
 * 注：枚举逻辑有一条关于偶数编号与奇数编号 OID 的
 * 特例规则，但这里并不考虑该规则；适用该规
 * 则时本例程根本不应被调用。
 */
int
compare_values_of_enum(TypeCacheEntry *tcache, Oid arg1, Oid arg2)
{
	TypeCacheEnumData *enumdata;
	EnumItem   *item1;
	EnumItem   *item2;

	/*
	 * OID 相等则值必然相等 --- 这种情形多半已由调用者
	 * 处理过了，但仍不妨检查。
	 */
	if (arg1 == arg2)
		return 0;

	/* 首次进入则装载缓存 */
	if (tcache->enumData == NULL)
		load_enum_cache_data(tcache);
	enumdata = tcache->enumData;

	/*
	 * 如果两个 OID 都已知有序，就可以直接比较它们。
	 */
	if (enum_known_sorted(enumdata, arg1) &&
		enum_known_sorted(enumdata, arg2))
	{
		if (arg1 < arg2)
			return -1;
		else
			return 1;
	}

	/*
	 * 慢路径：我们必须查明它们真实的排序位置。
	 */
	item1 = find_enumitem(enumdata, arg1);
	item2 = find_enumitem(enumdata, arg2);

	if (item1 == NULL || item2 == NULL)
	{
		/*
		 * 找不到其中一个或两个值。这说明枚举在我们
		 * 脚下变了，所以要重新初始化缓存并重试。这
		 * 条路径中不再费心重试已知有序情形。
		 */
		load_enum_cache_data(tcache);
		enumdata = tcache->enumData;

		item1 = find_enumitem(enumdata, arg1);
		item2 = find_enumitem(enumdata, arg2);

		/*
		 * 如果仍找不到这些值就报错：数据必已损
		 * 坏
		 */
		if (item1 == NULL)
			elog(ERROR, "enum value %u not found in cache for enum %s",
				 arg1, format_type_be(tcache->type_id));
		if (item2 == NULL)
			elog(ERROR, "enum value %u not found in cache for enum %s",
				 arg2, format_type_be(tcache->type_id));
	}

	if (item1->sort_order < item2->sort_order)
		return -1;
	else if (item1->sort_order > item2->sort_order)
		return 1;
	else
		return 0;
}

/*
 * 装载（重载）typcache 条目 enumData 成员。
 */
static void
load_enum_cache_data(TypeCacheEntry *tcache)
{
	TypeCacheEnumData *enumdata;
	Relation	enum_rel;
	SysScanDesc enum_scan;
	HeapTuple	enum_tuple;
	ScanKeyData skey;
	EnumItem   *items;
	int			numitems;
	int			maxitems;
	Oid			bitmap_base;
	Bitmapset  *bitmap;
	MemoryContext oldcxt;
	int			bm_size,
				start_pos;

	/* 检查这确实是个枚举 */
	if (tcache->typtype != TYPTYPE_ENUM)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("%s is not an enum",
						format_type_be(tcache->type_id))));

	/*
	 * 读取枚举类型成员的全部信息。我们先把信息收集
	 * 在调用者环境的工作内存中，然后再转移到
	 * CacheMemoryContext 的永久内存中。这样能把中途出
	 * 错时从 CacheMemoryContext 泄漏内存风险降到最
	 * 低。
	 */
	maxitems = 64;
	items = (EnumItem *) palloc(sizeof(EnumItem) * maxitems);
	numitems = 0;

	/* 扫描 pg_enum 找目标枚举类型的成员。 */
	ScanKeyInit(&skey,
				Anum_pg_enum_enumtypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tcache->type_id));

	enum_rel = table_open(EnumRelationId, AccessShareLock);
	enum_scan = systable_beginscan(enum_rel,
								   EnumTypIdLabelIndexId,
								   true, NULL,
								   1, &skey);

	while (HeapTupleIsValid(enum_tuple = systable_getnext(enum_scan)))
	{
		Form_pg_enum en = (Form_pg_enum) GETSTRUCT(enum_tuple);

		if (numitems >= maxitems)
		{
			maxitems *= 2;
			items = (EnumItem *) repalloc(items, sizeof(EnumItem) * maxitems);
		}
		items[numitems].enum_oid = en->oid;
		items[numitems].sort_order = en->enumsortorder;
		numitems++;
	}

	systable_endscan(enum_scan);
	table_close(enum_rel, AccessShareLock);

	/* 把条目排成 OID 顺序 */
	qsort(items, numitems, sizeof(EnumItem), enum_oid_cmp);

	/*
	 * 这里我们创建一个位图，列出枚举的已知有序、因而
	 * 可以仅凭 OID 比较来比较的 OID 子集。
	 *
	 * 这样做的理由是：枚举最初的 OID 必然有序，所以
	 * 总存在能用 OID 比较来比较的某个子集；而我们不
	 * 希望做不必要的二分搜索。
	 *
	 * 这带有一定试探性，识别出的 OID 子集可能并不正
	 * 是类型最初的那批。只要该子集是正确排序的，
	 * 就没问题。
	 */
	bitmap_base = InvalidOid;
	bitmap = NULL;
	bm_size = 1;				/* 只保存 ≥2 个 OID 的集合 */

	for (start_pos = 0; start_pos < numitems - 1; start_pos++)
	{
		/*
		 * 找出自 start_pos 起的最长有序子序列
		 */
		Bitmapset  *this_bitmap = bms_make_singleton(0);
		int			this_bm_size = 1;
		Oid			start_oid = items[start_pos].enum_oid;
		float4		prev_order = items[start_pos].sort_order;
		int			i;

		for (i = start_pos + 1; i < numitems; i++)
		{
			Oid			offset;

			offset = items[i].enum_oid - start_oid;
			/* 若位图会太大则退出；截断值是任意的 */
			if (offset >= 8192)
				break;
			/* 若条目有序则纳入它 */
			if (items[i].sort_order > prev_order)
			{
				prev_order = items[i].sort_order;
				this_bitmap = bms_add_member(this_bitmap, (int) offset);
				this_bm_size++;
			}
		}

		/* 若大于此前最佳则记住它 */
		if (this_bm_size > bm_size)
		{
			bms_free(bitmap);
			bitmap_base = start_oid;
			bitmap = this_bitmap;
			bm_size = this_bm_size;
		}
		else
			bms_free(this_bitmap);

		/*
		 * 如果剩余列表中不可能找到更长的序列就完成。典
		 * 型情形中首轮迭代就会发生，故我们直接在
		 * 运行中创建位图，而不是对列表再做
		 * 第二遍扫描。
		 */
		if (bm_size >= (numitems - start_pos - 1))
			break;
	}

	/* 把数据复制到 CacheMemoryContext */
	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);
	enumdata = (TypeCacheEnumData *)
		palloc(offsetof(TypeCacheEnumData, enum_values) +
			   numitems * sizeof(EnumItem));
	enumdata->bitmap_base = bitmap_base;
	enumdata->sorted_values = bms_copy(bitmap);
	enumdata->num_values = numitems;
	memcpy(enumdata->enum_values, items, numitems * sizeof(EnumItem));
	MemoryContextSwitchTo(oldcxt);

	pfree(items);
	bms_free(bitmap);

	/* 再把完成的缓存结构体链接进 typcache */
	if (tcache->enumData != NULL)
		pfree(tcache->enumData);
	tcache->enumData = enumdata;
}

/*
 * 若存在，找出具有给定 OID 的 EnumItem
 */
static EnumItem *
find_enumitem(TypeCacheEnumData *enumdata, Oid arg)
{
	EnumItem	srch;

	/* Solaris 某些版本上零条目 bsearch 会转储核心 */
	if (enumdata->num_values <= 0)
		return NULL;

	srch.enum_oid = arg;
	return bsearch(&srch, enumdata->enum_values, enumdata->num_values,
				   sizeof(EnumItem), enum_oid_cmp);
}

/*
 * 按 OID 排序 EnumItem 的 qsort 比较函数
 */
static int
enum_oid_cmp(const void *left, const void *right)
{
	const EnumItem *l = (const EnumItem *) left;
	const EnumItem *r = (const EnumItem *) right;

	if (l->enum_oid < r->enum_oid)
		return -1;
	else if (l->enum_oid > r->enum_oid)
		return 1;
	else
		return 0;
}

/*
 * 把 'tupdesc' 复制到 'area' 新分配的共享内存，把其 typmod
 * 设为给定值并返回 dsa_pointer。
 */
static dsa_pointer
share_tupledesc(dsa_area *area, TupleDesc tupdesc, uint32 typmod)
{
	dsa_pointer shared_dp;
	TupleDesc	shared;

	shared_dp = dsa_allocate(area, TupleDescSize(tupdesc));
	shared = (TupleDesc) dsa_get_address(area, shared_dp);
	TupleDescCopy(shared, tupdesc);
	shared->tdtypmod = typmod;

	return shared_dp;
}

/*
 * 如果我们已挂接到某个 SharedRecordTypmodRegistry，就用
 * 它来查找或创建与 'tupdesc' 匹配的共享 TupleDesc。否则
 * 返回 NULL。本函数返回的元组描述符不带引用计数，并
 * 且至少存在到当前后端保持挂接于当前会话的时
 * 候为止。
 */
static TupleDesc
find_or_make_matching_shared_tupledesc(TupleDesc tupdesc)
{
	TupleDesc	result;
	SharedRecordTableKey key;
	SharedRecordTableEntry *record_table_entry;
	SharedTypmodTableEntry *typmod_table_entry;
	dsa_pointer shared_dp;
	bool		found;
	uint32		typmod;

	/* 若根本未挂接，无事可做。 */
	if (CurrentSession->shared_typmod_registry == NULL)
		return NULL;

	/* 尝试在记录表中找到匹配的元组描述符。 */
	key.shared = false;
	key.u.local_tupdesc = tupdesc;
	record_table_entry = (SharedRecordTableEntry *)
		dshash_find(CurrentSession->shared_record_table, &key, false);
	if (record_table_entry)
	{
		Assert(record_table_entry->key.shared);
		dshash_release_lock(CurrentSession->shared_record_table,
							record_table_entry);
		result = (TupleDesc)
			dsa_get_address(CurrentSession->area,
							record_table_entry->key.u.shared_tupdesc);
		Assert(result->tdrefcount == -1);

		return result;
	}

	/* 分配新的 typmod 编号。若中途出错它就白费了。 */
	typmod = (int)
		pg_atomic_fetch_add_u32(&CurrentSession->shared_typmod_registry->next_typmod,
								1);

	/* 把 TupleDesc 复制到共享内存。 */
	shared_dp = share_tupledesc(CurrentSession->area, tupdesc, typmod);

	/*
	 * 在 typmod 表中创建条目，让其他后端理解此
	 * typmod 号。
	 */
	PG_TRY();
	{
		typmod_table_entry = (SharedTypmodTableEntry *)
			dshash_find_or_insert(CurrentSession->shared_typmod_table,
								  &typmod, &found);
		if (found)
			elog(ERROR, "cannot create duplicate shared record typmod");
	}
	PG_CATCH();
	{
		dsa_free(CurrentSession->area, shared_dp);
		PG_RE_THROW();
	}
	PG_END_TRY();
	typmod_table_entry->typmod = typmod;
	typmod_table_entry->shared_tupdesc = shared_dp;
	dshash_release_lock(CurrentSession->shared_typmod_table,
						typmod_table_entry);

	/*
	 * 最后在记录表中创建条目，让其他拥有匹配元组描
	 * 述符的人能复用该 typmod。
	 */
	record_table_entry = (SharedRecordTableEntry *)
		dshash_find_or_insert(CurrentSession->shared_record_table, &key,
							  &found);
	if (found)
	{
		/*
		 * 自我们上次检查后，有人并发插入了匹配的元组描
		 * 述符。改用那个。
		 */
		dshash_release_lock(CurrentSession->shared_record_table,
							record_table_entry);

		/* 不妨释放我们自己创建的那个所占的空间。 */
		found = dshash_delete_key(CurrentSession->shared_typmod_table,
								  &typmod);
		Assert(found);
		dsa_free(CurrentSession->area, shared_dp);

		/* 返回找到的那个。 */
		Assert(record_table_entry->key.shared);
		result = (TupleDesc)
			dsa_get_address(CurrentSession->area,
							record_table_entry->key.u.shared_tupdesc);
		Assert(result->tdrefcount == -1);

		return result;
	}

	/* 存下并返回它。 */
	record_table_entry->key.shared = true;
	record_table_entry->key.u.shared_tupdesc = shared_dp;
	dshash_release_lock(CurrentSession->shared_record_table,
						record_table_entry);
	result = (TupleDesc)
		dsa_get_address(CurrentSession->area, shared_dp);
	Assert(result->tdrefcount == -1);

	return result;
}

/*
 * DSM 分离时调用的钩子，用于忘掉当前共享记录
 * typmod 基础设施。目前 leader 与工作进程都在用它。
 */
static void
shared_record_typmod_registry_detach(dsm_segment *segment, Datum datum)
{
	/* 这里要小心：可能我们尚未完成初始化。 */
	if (CurrentSession->shared_record_table != NULL)
	{
		dshash_detach(CurrentSession->shared_record_table);
		CurrentSession->shared_record_table = NULL;
	}
	if (CurrentSession->shared_typmod_table != NULL)
	{
		dshash_detach(CurrentSession->shared_typmod_table);
		CurrentSession->shared_typmod_table = NULL;
	}
	CurrentSession->shared_typmod_registry = NULL;
}
