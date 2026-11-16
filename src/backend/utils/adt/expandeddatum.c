/*-------------------------------------------------------------------------
 *
 * expandeddatum.c
 *	  支持"展开"值表示形式的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/expandeddatum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/expandeddatum.h"
#include "utils/memutils.h"

/*
 * DatumGetEOHP
 *
 * 给定一个作为展开对象引用的 Datum，提取出其指针。
 *
 * 这一步骤略显繁琐，因为指针可能未正确对齐；
 * 比 VARATT_EXTERNAL_GET_POINTER()。
 */
ExpandedObjectHeader *
DatumGetEOHP(Datum d)
{
	varattrib_1b_e *datum = (varattrib_1b_e *) DatumGetPointer(d);
	varatt_expanded ptr;

	Assert(VARATT_IS_EXTERNAL_EXPANDED(datum));
	memcpy(&ptr, VARDATA_EXTERNAL(datum), sizeof(ptr));
	Assert(VARATT_IS_EXPANDED_HEADER(ptr.eohptr));
	return ptr.eohptr;
}

/*
 * EOH_init_header
 *
 * 初始化展开对象的公共头部。
 *
 * 它主要封装的是对 TOAST 指针的初始化。
 */
void
EOH_init_header(ExpandedObjectHeader *eohptr,
				const ExpandedObjectMethods *methods,
				MemoryContext obj_context)
{
	varatt_expanded ptr;

	eohptr->vl_len_ = EOH_HEADER_MAGIC;
	eohptr->eoh_methods = methods;
	eohptr->eoh_context = obj_context;

	ptr.eohptr = eohptr;

	SET_VARTAG_EXTERNAL(eohptr->eoh_rw_ptr, VARTAG_EXPANDED_RW);
	memcpy(VARDATA_EXTERNAL(eohptr->eoh_rw_ptr), &ptr, sizeof(ptr));

	SET_VARTAG_EXTERNAL(eohptr->eoh_ro_ptr, VARTAG_EXPANDED_RO);
	memcpy(VARDATA_EXTERNAL(eohptr->eoh_ro_ptr), &ptr, sizeof(ptr));
}

/*
 * EOH_get_flat_size
 * EOH_flatten_into
 *
 * 调用展开对象"方法" (methods) 的便捷函数。
 */

Size
EOH_get_flat_size(ExpandedObjectHeader *eohptr)
{
	return eohptr->eoh_methods->get_flat_size(eohptr);
}

void
EOH_flatten_into(ExpandedObjectHeader *eohptr,
				 void *result, Size allocated_size)
{
	eohptr->eoh_methods->flatten_into(eohptr, result, allocated_size);
}

/*
 * 若 Datum 表示读写展开对象，则将其改为只读。
 * 否则返回原 Datum。
 *
 * 调用方必须确保 datum 是非空 varlena 值。通常
 * 经由 MakeExpandedObjectReadOnly() 调用，它会检查。
 */
Datum
MakeExpandedObjectReadOnlyInternal(Datum d)
{
	ExpandedObjectHeader *eohptr;

	/* 若非读写展开对象指针则无事可做 */
	if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)))
		return d;

	/* 现在可安全提取对象指针 */
	eohptr = DatumGetEOHP(d);

	/* 返回内置只读指针，而非传入的指针 */
	return EOHPGetRODatum(eohptr);
}

/*
 * 将展开对象的所有权转移给新的父内存上下文。
 * 该对象必须由读写指针引用，而我们返回的是
 * 它的"标准"读写指针，该指针的生命周期必定
 * 与对象本身相同。（传入的指针未必如此，而且
 * 若非该指针则无论如何都无法提供唯一标识。）
 */
Datum
TransferExpandedObject(Datum d, MemoryContext new_parent)
{
	ExpandedObjectHeader *eohptr = DatumGetEOHP(d);

	/* 断言调用方给读写指针 */
	Assert(VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)));

	/* 转移所有权 */
	MemoryContextSetParent(eohptr->eoh_context, new_parent);

	/* 返回该对象的标准读写指针 */
	return EOHPGetRWDatum(eohptr);
}

/*
 * 删除一个展开对象（必须由读写指针引用）。
 */
void
DeleteExpandedObject(Datum d)
{
	ExpandedObjectHeader *eohptr = DatumGetEOHP(d);

	/* 断言调用方给读写指针 */
	Assert(VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(d)));

	/* 清除 */
	MemoryContextDelete(eohptr->eoh_context);
}
