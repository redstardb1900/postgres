/*-------------------------------------------------------------------------
 *
 * toast_helper.c
 *	  供表 AM 实现变长属性的压缩存储或
 *    行外存储的辅助函数。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/table/toast_helper.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/table.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "catalog/pg_type_d.h"
#include "varatt.h"


/*
 * 准备对元组做 TOAST。
 *
 * tupleDesc、toast_values 和 toast_isnull 是必需参数；它们
 * 提供待做 TOAST 的元组所需的细节。
 *
 * 新插入的元组应把 toast_oldvalues 和 toast_oldisnull 置为 NULL；
 * 更新时，它们应描述已有元组。
 *
 * 这些数组的长度都应等于 tupleDesc->natts。
 *
 * 返回时，toast_flags 和 toast_attr 都已初始化。
 * toast_flags 只是单个 uint8，但 toast_attr 是调用方提供的
 * 数组，长度等于 tupleDesc->natts。调用本函数之前，调用方不必
 * 对该数组做任何初始化。
 */
void
toast_tuple_init(ToastTupleContext *ttc)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			i;

	ttc->ttc_flags = 0;

	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		struct varlena *old_value;
		struct varlena *new_value;

		ttc->ttc_attr[i].tai_colflags = 0;
		ttc->ttc_attr[i].tai_oldexternal = NULL;
		ttc->ttc_attr[i].tai_compression = att->attcompression;

		if (ttc->ttc_oldvalues != NULL)
		{
			/*
			 * UPDATE 时取得该属性的旧值和新值
			 */
			old_value =
				(struct varlena *) DatumGetPointer(ttc->ttc_oldvalues[i]);
			new_value =
				(struct varlena *) DatumGetPointer(ttc->ttc_values[i]);

			/*
			 * 若旧值存放在磁盘上，检查它是否已改变，从而
			 * 稍后必须删除它。
			 */
			if (att->attlen == -1 && !ttc->ttc_oldisnull[i] &&
				VARATT_IS_EXTERNAL_ONDISK(old_value))
			{
				if (ttc->ttc_isnull[i] ||
					!VARATT_IS_EXTERNAL_ONDISK(new_value) ||
					memcmp((char *) old_value, (char *) new_value,
						   VARSIZE_EXTERNAL(old_value)) != 0)
				{
					/*
					 * 更新之后不再需要旧的
					 * 外部存储值
					 */
					ttc->ttc_attr[i].tai_colflags |= TOASTCOL_NEEDS_DELETE_OLD;
					ttc->ttc_flags |= TOAST_NEEDS_DELETE_OLD;
				}
				else
				{
					/*
					 * 本次更新没有改变该属性，因此复用
					 * 新元组中指向旧值的
					 * 原始引用。
					 */
					ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
					continue;
				}
			}
		}
		else
		{
			/*
			 * INSERT 时直接取得新值
			 */
			new_value = (struct varlena *) DatumGetPointer(ttc->ttc_values[i]);
		}

		/*
		 * 处理 NULL 属性
		 */
		if (ttc->ttc_isnull[i])
		{
			ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
			ttc->ttc_flags |= TOAST_HAS_NULLS;
			continue;
		}

		/*
		 * 接下来看变长属性
		 */
		if (att->attlen == -1)
		{
			/*
			 * 若表的属性声明始终为 PLAIN，就强制如此。
			 */
			if (att->attstorage == TYPSTORAGE_PLAIN)
				ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;

			/*
			 * 上面已经处理过 UPDATE，因此仍能在元组里找到的任何外部值
			 * 必定属于别人，我们无法复用
			 * （这也包括行外的内存中 datum）。
			 * 把它取回（不做解压，除非正在强制
			 * PLAIN 存储）。必要时，下面会把它作为新的
			 * 外部值推出去。
			 */
			if (VARATT_IS_EXTERNAL(new_value))
			{
				ttc->ttc_attr[i].tai_oldexternal = new_value;
				if (att->attstorage == TYPSTORAGE_PLAIN)
					new_value = detoast_attr(new_value);
				else
					new_value = detoast_external_attr(new_value);
				ttc->ttc_values[i] = PointerGetDatum(new_value);
				ttc->ttc_attr[i].tai_colflags |= TOASTCOL_NEEDS_FREE;
				ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
			}

			/*
			 * 记住该属性的大小
			 */
			ttc->ttc_attr[i].tai_size = VARSIZE_ANY(new_value);
		}
		else
		{
			/*
			 * 不是变长属性，始终用普通存储
			 */
			ttc->ttc_attr[i].tai_colflags |= TOASTCOL_IGNORE;
		}
	}
}

/*
 * 找出满足特定条件的最大变长属性。
 *
 * 相关列不得标为 TOASTCOL_IGNORE；若传入的
 * for_compression 标志为 true，则也不得标为
 * TOASTCOL_INCOMPRESSIBLE。
 *
 * check_main 为 false 时，列的 attstorage 必须是 EXTERNAL 或 EXTENDED；
 * check_main 为 true 时，attstorage 必须是 MAIN。
 *
 * 列的最小尺寸必须是 MAXALIGN(TOAST_POINTER_SIZE)；
 * 否则，压缩它预期没有收益。
 *
 * 返回值是最大合适列的下标，
 * 若没有则为 -1。
 */
int
toast_tuple_find_biggest_attribute(ToastTupleContext *ttc,
								   bool for_compression, bool check_main)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			biggest_attno = -1;
	int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
	int32		skip_colflags = TOASTCOL_IGNORE;
	int			i;

	if (for_compression)
		skip_colflags |= TOASTCOL_INCOMPRESSIBLE;

	for (i = 0; i < numAttrs; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);

		if ((ttc->ttc_attr[i].tai_colflags & skip_colflags) != 0)
			continue;
		if (VARATT_IS_EXTERNAL(DatumGetPointer(ttc->ttc_values[i])))
			continue;			/* 不可能发生，toast_action 会是 PLAIN */
		if (for_compression &&
			VARATT_IS_COMPRESSED(DatumGetPointer(ttc->ttc_values[i])))
			continue;
		if (check_main && att->attstorage != TYPSTORAGE_MAIN)
			continue;
		if (!check_main && att->attstorage != TYPSTORAGE_EXTENDED &&
			att->attstorage != TYPSTORAGE_EXTERNAL)
			continue;

		if (ttc->ttc_attr[i].tai_size > biggest_size)
		{
			biggest_attno = i;
			biggest_size = ttc->ttc_attr[i].tai_size;
		}
	}

	return biggest_attno;
}

/*
 * 尝试压缩某个属性。
 *
 * 若发现该属性不可压缩，就把它标成这样。
 */
void
toast_tuple_try_compression(ToastTupleContext *ttc, int attribute)
{
	Datum	   *value = &ttc->ttc_values[attribute];
	Datum		new_value;
	ToastAttrInfo *attr = &ttc->ttc_attr[attribute];

	new_value = toast_compress_datum(*value, attr->tai_compression);

	if (DatumGetPointer(new_value) != NULL)
	{
		/* 压缩成功 */
		if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
			pfree(DatumGetPointer(*value));
		*value = new_value;
		attr->tai_colflags |= TOASTCOL_NEEDS_FREE;
		attr->tai_size = VARSIZE(DatumGetPointer(*value));
		ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
	}
	else
	{
		/* 不可压缩，后续压缩轮次中忽略 */
		attr->tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
	}
}

/*
 * 把属性移到外部存储。
 */
void
toast_tuple_externalize(ToastTupleContext *ttc, int attribute, int options)
{
	Datum	   *value = &ttc->ttc_values[attribute];
	Datum		old_value = *value;
	ToastAttrInfo *attr = &ttc->ttc_attr[attribute];

	attr->tai_colflags |= TOASTCOL_IGNORE;
	*value = toast_save_datum(ttc->ttc_rel, old_value, attr->tai_oldexternal,
							  options);
	if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
		pfree(DatumGetPointer(old_value));
	attr->tai_colflags |= TOASTCOL_NEEDS_FREE;
	ttc->ttc_flags |= (TOAST_NEEDS_CHANGE | TOAST_NEEDS_FREE);
}

/*
 * 一个元组做过 TOAST 之后，做相应的清理。
 */
void
toast_tuple_cleanup(ToastTupleContext *ttc)
{
	TupleDesc	tupleDesc = ttc->ttc_rel->rd_att;
	int			numAttrs = tupleDesc->natts;

	/*
	 * 释放已分配的临时值
	 */
	if ((ttc->ttc_flags & TOAST_NEEDS_FREE) != 0)
	{
		int			i;

		for (i = 0; i < numAttrs; i++)
		{
			ToastAttrInfo *attr = &ttc->ttc_attr[i];

			if ((attr->tai_colflags & TOASTCOL_NEEDS_FREE) != 0)
				pfree(DatumGetPointer(ttc->ttc_values[i]));
		}
	}

	/*
	 * 删除旧元组中的外部值
	 */
	if ((ttc->ttc_flags & TOAST_NEEDS_DELETE_OLD) != 0)
	{
		int			i;

		for (i = 0; i < numAttrs; i++)
		{
			ToastAttrInfo *attr = &ttc->ttc_attr[i];

			if ((attr->tai_colflags & TOASTCOL_NEEDS_DELETE_OLD) != 0)
				toast_delete_datum(ttc->ttc_rel, ttc->ttc_oldvalues[i], false);
		}
	}
}

/*
 * 检查外部存储的属性，并把它们从二级
 * 关系中删除。
 */
void
toast_delete_external(Relation rel, Datum *values, bool *isnull,
					  bool is_speculative)
{
	TupleDesc	tupleDesc = rel->rd_att;
	int			numAttrs = tupleDesc->natts;
	int			i;

	for (i = 0; i < numAttrs; i++)
	{
		if (TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			Datum		value = values[i];

			if (isnull[i])
				continue;
			else if (VARATT_IS_EXTERNAL_ONDISK(value))
				toast_delete_datum(rel, value, is_speculative);
		}
	}
}
