/*-------------------------------------------------------------------------
 *
 * expandeddatum.h
 *	  访问"扩展"值表示的声明。
 *
 * 复杂数据类型，尤其数组与记录等容器类型，盘上
 * 表示通常紧凑但不便修改。而且一旦修改，往往得
 * 重拷其余全部，效率极低。故提供仅存内存、更利
 * 于计算而非存储的"扩展"表示。盘上格式称为该类
 * 型的"扁平"表示，因其须为连续字节块——但类型
 * 也可有非连续扩展表示。类型须提供把扩展表示
 * 译回扁平形式的手段。
 * 见上。
 * 见上。
 * 见上。
 * 见上。
 *
 * 扩展对象意在跨多次操作存活，但不必极长久；例如
 * 可为 PL/pgSQL 过程中的局部变量。相对盘上形式多出
 * 的体积是值得的折中。
 * 见上。
 *
 * 对扩展对象的引用是一种 TOAST 指针。
 * 因 Postgres 的长期约定，这意味着此类对象的扁平
 * 形式必须始终是 varlena 对象。实践中无此限制。
 * 见上。
 *
 * 扩展对象实际有两种 TOAST 指针：只读与读写指针。
 * 持有后者即授权函数就地改值，而无须像通常那样
 * 先拷贝。函数应始终为其创建的任何新扩展对象
 * 返回读写指针。就地改参数值的函数须确保中途
 * 失败时不破坏旧值。
 * 见上。
 * 见上。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/expandeddatum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPANDEDDATUM_H
#define EXPANDEDDATUM_H

#include "varatt.h"

/* 含指向扩展对象指针的 EXTERNAL datum 的大小 */
#define EXPANDED_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_expanded))

/*
 * 任何扩展对象都必须提供的"方法"。
 *
 * get_flat_size：计算扁平表示所需空间（含头的总
 * 计）。
 *
 * flatten_into：在调用方分配的 *result 处构造扁平
 * 表示，大小为 allocated_size（总是此前 get_flat_size
 * 的返回值；传入以供核对）。
 *
 * 扁平表示必须是合法的内联、未压缩、4 字节头
 * varlena 对象。
 *
 * 注意：从扩展 datum 构造堆元组会调用 get_flat_size
 * 两次，故宜确保其开销不太大。
 * 见上。
 */
typedef Size (*EOM_get_flat_size_method) (ExpandedObjectHeader *eohptr);
typedef void (*EOM_flatten_into_method) (ExpandedObjectHeader *eohptr,
										 void *result, Size allocated_size);

/* 扩展对象方法的函数指针结构 */
typedef struct ExpandedObjectMethods
{
	EOM_get_flat_size_method get_flat_size;
	EOM_flatten_into_method flatten_into;
} ExpandedObjectMethods;

/*
 * 每个扩展对象须含此头；通常嵌在更大结构中，
 * 后者再加类型专用字段。
 *
 * 假定头及全部附属数据存于 eoh_context，故可删
 * 该上下文以释放对象，或改挂上下文以改变存储
 * 寿命。（原则上还可拥有额外资源，如 malloc
 * 存储，并用上下文复位回调在复位或删除
 * eoh_context 时释放。）
 * 见上。
 *
 * 我们在标准头内设两个 TOAST 指针：一读写、一只读。
 * 使函数无需额外分配即可返回任一种指针，尤其不必担
 * 心单独 palloc 的对象寿命是否够。但注意这些指针只是
 * 便利；出现在别处的指针对象仍合法。
 * 见上。
 * 见上。
 *
 * 此类型定义的声明在 postgres.h。
 */
struct ExpandedObjectHeader
{
	/* 伪 varlena 头 */
	int32		vl_len_;		/* 恒为 EOH_HEADER_MAGIC，见下 */

	/* 指向对象类型所需方法的指针 */
	const ExpandedObjectMethods *eoh_methods;

	/* 含此头及附属数据的内存上下文 */
	MemoryContext eoh_context;

	/* 本对象的标准 R/W TOAST 指针保存在此 */
	char		eoh_rw_ptr[EXPANDED_POINTER_SIZE];

	/* 本对象的标准 R/O TOAST 指针保存在此 */
	char		eoh_ro_ptr[EXPANDED_POINTER_SIZE];
};

/*
 * 尤其对只读函数，便于同时处理同类型的普通"扁平"
 * varlena 输入或扩展输入。为区分参数取数函数返回哪
 * 种情形，ExpandedObjectHeader 的首个 int32 恒为 -1
 * 代码中为 EOH_HEADER_MAGIC。因无任何 4 字节头
 * varlena 会以该四字节开头。注意：此技巧无法可靠区
 * 分 ExpandedObjectHeader 与短头对象。但若参数取数
 * 代码总是返回 4 字节头扁平对象或扩展对象，则工作
 * 良好。
 * 见上。
 */
#define EOH_HEADER_MAGIC (-1)
#define VARATT_IS_EXPANDED_HEADER(PTR) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header == (uint32) EOH_HEADER_MAGIC)

/*
 * 扩展对象的通用支持函数。
 * 其中更多日后或许值得内联。
 */

static inline Datum
EOHPGetRWDatum(const struct ExpandedObjectHeader *eohptr)
{
	return PointerGetDatum(eohptr->eoh_rw_ptr);
}

static inline Datum
EOHPGetRODatum(const struct ExpandedObjectHeader *eohptr)
{
	return PointerGetDatum(eohptr->eoh_ro_ptr);
}

/* Datum 是否表示可写的扩展对象？ */
#define DatumIsReadWriteExpandedObject(d, isnull, typlen) \
	(((isnull) || (typlen) != -1) ? false : \
	 VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))

#define MakeExpandedObjectReadOnly(d, isnull, typlen) \
	(((isnull) || (typlen) != -1) ? (d) : \
	 MakeExpandedObjectReadOnlyInternal(d))

extern ExpandedObjectHeader *DatumGetEOHP(Datum d);
extern void EOH_init_header(ExpandedObjectHeader *eohptr,
							const ExpandedObjectMethods *methods,
							MemoryContext obj_context);
extern Size EOH_get_flat_size(ExpandedObjectHeader *eohptr);
extern void EOH_flatten_into(ExpandedObjectHeader *eohptr,
							 void *result, Size allocated_size);
extern Datum MakeExpandedObjectReadOnlyInternal(Datum d);
extern Datum TransferExpandedObject(Datum d, MemoryContext new_parent);
extern void DeleteExpandedObject(Datum d);

#endif							/* EXPANDEDDATUM_H */
