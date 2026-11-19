/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  访问压缩及外部存储的 varlena 值。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

/*
 * 把 EXTERNAL datum 中可能未对齐的内容取到局部的
 * "struct varatt_external" toast 指针中。这本应只是
 * 一次 memcpy，但部分 gcc 版本似乎生成错误代码，
 * 其假定 datum 内容是对齐的。引入显式的中间变量
 * "varattrib_1b_e *" 似乎就能修复。
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* 含标准 TOAST 指针的 EXTERNAL datum 的大小 */
#define TOAST_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external))

/* 含间接指针的 EXTERNAL datum 的大小 */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/* ----------
 * detoast_external_attr() -
 *
 *		从 toast 关系取回外部存储的属性。
 *		不会解压它，即使它以压缩格式
 *		外部存储。
 * ----------
 */
extern struct varlena *detoast_external_attr(struct varlena *attr);

/* ----------
 * detoast_attr() -
 *
 *		完整还原一个属性，按需取回和/或
 *		解压。
 * ----------
 */
extern struct varlena *detoast_attr(struct varlena *attr);

/* ----------
 * detoast_attr_slice() -
 *
 *		只取回属性的指定部分。
 *		(处理属性存储的所有情形)
 * ----------
 */
extern struct varlena *detoast_attr_slice(struct varlena *attr,
										  int32 sliceoffset,
										  int32 slicelength);

/* ----------
 * toast_raw_datum_size -
 *
 *	返回 varlena 值的原始(还原后)大小
 * ----------
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	返回 varlena 值的存储大小
 * ----------
 */
extern Size toast_datum_size(Datum value);

#endif							/* DETOAST_H */
