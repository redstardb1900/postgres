/*-------------------------------------------------------------------------
 *
 * toast_helper.h
 *	  供表 AM 实现压缩或
 *   变长属性外置存储的辅助函数。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * src/include/access/toast_helper.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef TOAST_HELPER_H
#define TOAST_HELPER_H

#include "utils/rel.h"

/*
 * 正在 toast 的元组中某一列的信息。
 *
 * 注意：toast_action[i] 可取以下值：
 *		' '						默认处理
 *		TYPSTORAGE_PLAIN		已处理 --- 勿再动
 *		TYPSTORAGE_EXTENDED		不可压缩，但可外置
 *
 * 注意：toast_attr[i].tai_size 仅对 toast_action[i] 非 
 * TYPSTORAGE_PLAIN 的变长属性才有效。
 */
typedef struct
{
	struct varlena *tai_oldexternal;
	int32		tai_size;
	uint8		tai_colflags;
	char		tai_compression;
} ToastAttrInfo;

/*
 * 正在 toast 的某一元组的信息。
 */
typedef struct
{
	/*
	 * 调用 toast_tuple_init 前，调用方必须初始化下列
	 * 字段。各数组长度须等于 
	 * ttc_rel->rd_att->natts。插入时 ttc_oldvalues 与 
	 * ttc_oldisnull 字段应为 NULL。
	 */
	Relation	ttc_rel;		/* 含该元组的关系 */
	Datum	   *ttc_values;		/* 元组各列的值 */
	bool	   *ttc_isnull;		/* 元组各列的空值标志 */
	Datum	   *ttc_oldvalues;	/* 前一元组的值 */
	bool	   *ttc_oldisnull;	/* 前一元组的空值标志 */

	/*
	 * 调用 toast_tuple_init 前，调用方应将 ttc_attr 设为
	 * 指向 ToastAttrInfo 结构数组，长度等于 
	 * ttc_rel->rd_att->natts。数组内容不必
	 * 初始化。ttc_flags 也无须初始化。
	 */
	uint8		ttc_flags;
	ToastAttrInfo *ttc_attr;
} ToastTupleContext;

/*
 * 指示一次 TOAST 操作整体状态的标志。
 *
 * TOAST_NEEDS_DELETE_OLD 表示一个或多个旧 TOAST datum 需
 * 删除。
 *
 * TOAST_NEEDS_FREE 表示一个或多个 TOAST 值需释放。
 *
 * TOAST_HAS_NULLS 表示正在 toast 的元组中发现了空值。
 *
 * TOAST_NEEDS_CHANGE 表示需要构造新元组；换言之，
 * toaster 做了某些事。
 */
#define TOAST_NEEDS_DELETE_OLD				0x0001
#define TOAST_NEEDS_FREE					0x0002
#define TOAST_HAS_NULLS						0x0004
#define TOAST_NEEDS_CHANGE					0x0008

/*
 * 指示 TOAST 操作就某列而言的
 * 状态标志。
 *
 * TOASTCOL_NEEDS_DELETE_OLD 表示该列的旧 TOAST datum 需
 * 删除。
 *
 * TOASTCOL_NEEDS_FREE 表示该列的值需
 * 释放。
 *
 * TOASTCOL_IGNORE 表示 toaster 不应再处理
 * 该列。
 *
 * TOASTCOL_INCOMPRESSIBLE 表示已判明该列
 * 不可压缩，但可外置。
 */
#define TOASTCOL_NEEDS_DELETE_OLD			TOAST_NEEDS_DELETE_OLD
#define TOASTCOL_NEEDS_FREE					TOAST_NEEDS_FREE
#define TOASTCOL_IGNORE						0x0010
#define TOASTCOL_INCOMPRESSIBLE				0x0020

extern void toast_tuple_init(ToastTupleContext *ttc);
extern int	toast_tuple_find_biggest_attribute(ToastTupleContext *ttc,
											   bool for_compression,
											   bool check_main);
extern void toast_tuple_try_compression(ToastTupleContext *ttc, int attribute);
extern void toast_tuple_externalize(ToastTupleContext *ttc, int attribute,
									int options);
extern void toast_tuple_cleanup(ToastTupleContext *ttc);

extern void toast_delete_external(Relation rel, Datum *values, bool *isnull,
								  bool is_speculative);

#endif
