/*-------------------------------------------------------------------------
 *
 * heaptoast.c
 *	  堆专用的定义：变长属性的外部存储
 *	  与压缩存储。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heaptoast.c
 *
 *
 * 接口例程（INTERFACE ROUTINES）
 *		heap_toast_insert_or_update -
 *			尝试通过压缩或把属性移出，
 *			使给定元组放进一个页面
 *
 *		heap_toast_delete -
 *			元组被删除时回收 TOAST 存储
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "utils/fmgroids.h"


/* ----------
 * heap_toast_delete -
 *
 *	在 DELETE 时级联删除 TOAST 项
 * ----------
 */
void
heap_toast_delete(Relation rel, HeapTuple oldtup, bool is_speculative)
{
	TupleDesc	tupleDesc;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];

	/*
	 * 我们只应被普通关系或
	 * 物化视图的元组调用——在 TOAST 关系上递归是坏事。
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * 取得元组描述符，并把元组拆成各个属性。
	 *
	 * 注意：这里用 heap_deform_tuple() 还是只对 varlena 列
	 * 调用 heap_getattr()，尚可商榷。若 varlena 列很少、
	 * 非 varlena 列很多，后者可能更快。不过
	 * heap_deform_tuple 的代价只是 O(N)，而 heap_getattr 方式
	 * 在 varlena 列很多时会达到 O(N^2)，因此
	 * 更稳妥的是偏向线性代价。（若连一个
	 * varlena 列都没有，我们根本不会走到这里。）
	 */
	tupleDesc = rel->rd_att;

	Assert(tupleDesc->natts <= MaxHeapAttributeNumber);
	heap_deform_tuple(oldtup, tupleDesc, toast_values, toast_isnull);

	/* 做真正的工作。*/
	toast_delete_external(rel, toast_values, toast_isnull, is_speculative);
}


/* ----------
 * heap_toast_insert_or_update -
 *
 *	删除不再使用的 TOAST 项，并创建新的项，
 *	使新元组在 INSERT 或 UPDATE 时放得下
 *
 * 输入：
 *	newtup：准备插入的候选新元组
 *	oldtup：UPDATE 的旧行版本，INSERT 时为 NULL
 *	options：要传给 heap_insert() 的 TOAST 行选项
 * 结果：
 *	若不需要做 TOAST，则为 newtup；否则是 palloc 出来的修改后元组，
 *	真正应当存下去的就是它
 *
 * 注意：newtup 与 oldtup 都不会被修改。这是相对
 * 本例程 8.1 之前 API 的一处变更。
 * ----------
 */
HeapTuple
heap_toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
							int options)
{
	HeapTuple	result_tuple;
	TupleDesc	tupleDesc;
	int			numAttrs;

	Size		maxDataLen;
	Size		hoff;

	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	ToastAttrInfo toast_attr[MaxHeapAttributeNumber];
	ToastTupleContext ttc;

	/*
	 * 忽略 INSERT_SPECULATIVE 选项。推测性插入与超级
	 * 删除只是按普通方式插入/删除 TOAST 值。
	 * 在这里处理似乎最简单，而不是放到可能有的多个
	 * 调用方那里。
	 */
	options &= ~HEAP_INSERT_SPECULATIVE;

	/*
	 * 我们只应被普通关系或
	 * 物化视图的元组调用——在 TOAST 关系上递归是坏事。
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * 取得元组描述符，并把 tuple(s) 拆成各个字段。
	 */
	tupleDesc = rel->rd_att;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(newtup, tupleDesc, toast_values, toast_isnull);
	if (oldtup != NULL)
		heap_deform_tuple(oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);

	/* ----------
	 * 为做 TOAST 做准备
	 * ----------
	 */
	ttc.ttc_rel = rel;
	ttc.ttc_values = toast_values;
	ttc.ttc_isnull = toast_isnull;
	if (oldtup == NULL)
	{
		ttc.ttc_oldvalues = NULL;
		ttc.ttc_oldisnull = NULL;
	}
	else
	{
		ttc.ttc_oldvalues = toast_oldvalues;
		ttc.ttc_oldisnull = toast_oldisnull;
	}
	ttc.ttc_attr = toast_attr;
	toast_tuple_init(&ttc);

	/* ----------
	 * 压缩和/或外存，直到数据放进目标长度
	 *
	 *	1：行内压缩 attstorage 为 EXTENDED 的属性，并把很大的
	 *	   attstorage 为 EXTENDED 或 EXTERNAL 的属性立刻
	 *	   存到外部
	 *	2：把 attstorage 为 EXTENDED 或 EXTERNAL 的属性存到外部
	 *	3：行内压缩 attstorage 为 MAIN 的属性
	 *	4：把 attstorage 为 MAIN 的属性存到外部
	 * ----------
	 */

	/* 计算头部开销——这应当与 heap_form_tuple() 一致*/
	hoff = SizeofHeapTupleHeader;
	if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
		hoff += BITMAPLEN(numAttrs);
	hoff = MAXALIGN(hoff);
	/* 接着换成对元组数据大小的限制*/
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/*
	 * 寻找 attstorage 为 EXTENDED、可以压缩的属性。同时找出
	 * attstorage 为 EXTENDED 或 EXTERNAL 的大属性，并把它们
	 * 存到外部。
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, false);
		if (biggest_attno < 0)
			break;

		/*
		 * 若其 attstorage 为 EXTENDED，尝试行内压缩
		 */
		if (TupleDescAttr(tupleDesc, biggest_attno)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(&ttc, biggest_attno);
		else
		{
			/*
			 * attstorage 为 EXTERNAL，后续压缩
			 * 轮次中忽略
			 */
			toast_attr[biggest_attno].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
		}

		/*
		 * 若这个值本身（压缩之后，若有压缩）就超过 maxDataLen，
		 * 就尽可能立刻把它推到 TOAST 表。
		 * 这样在常见的一个长字段加几个短字段的情形下，
		 * 可以避免无谓地压缩其他字段。
		 *
		 * XXX 阈值也许应当小于 maxDataLen？
		 */
		if (toast_attr[biggest_attno].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 第二步，寻找 attstorage 为 EXTENDED 或 EXTERNAL、
	 * 仍在行内的属性，并把它们改成外部存储。但若没有
	 * 可推入的 TOAST 表，则跳过这一步。
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, false);
		if (biggest_attno < 0)
			break;
		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 第 3 轮——这次把存储方式为 MAIN 的属性纳入
	 * 压缩
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_try_compression(&ttc, biggest_attno);
	}

	/*
	 * 最后把类型为 MAIN 的属性存到外部。此时我们
	 * 提高目标元组大小，这样 MAIN 属性不会被存到
	 * 外部，除非确实有必要。
	 */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * 若我们把任何值做成了 TOAST，就需要用变更后的值
	 * 构造一个新的堆元组。
	 */
	if ((ttc.ttc_flags & TOAST_NEEDS_CHANGE) != 0)
	{
		HeapTupleHeader olddata = newtup->t_data;
		HeapTupleHeader new_data;
		int32		new_header_len;
		int32		new_data_len;
		int32		new_tuple_len;

		/*
		 * 计算元组的新大小。
		 *
		 * 注意：我们以前在这里假定旧元组的 t_hoff 必须等于
		 * new_header_len，但那是错的。旧元组
		 * 的 natts 可能小于当前值，若它存入之后做过 ALTER
		 * TABLE ADD COLUMN；那会导致对空值位图大小，
		 * 甚至对究竟要不要空值位图，得出
		 * 不同的结论。
		 */
		new_header_len = SizeofHeapTupleHeader;
		if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
			new_header_len += BITMAPLEN(numAttrs);
		new_header_len = MAXALIGN(new_header_len);
		new_data_len = heap_compute_data_size(tupleDesc,
											  toast_values, toast_isnull);
		new_tuple_len = new_header_len + new_data_len;

		/*
		 * 分配并把所需空间清零，并填写 HeapTupleData 字段。
		 */
		result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_tuple_len);
		result_tuple->t_len = new_tuple_len;
		result_tuple->t_self = newtup->t_self;
		result_tuple->t_tableOid = newtup->t_tableOid;
		new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
		result_tuple->t_data = new_data;

		/*
		 * 复制现有元组头，但调整 natts 与 t_hoff。
		 */
		memcpy(new_data, olddata, SizeofHeapTupleHeader);
		HeapTupleHeaderSetNatts(new_data, numAttrs);
		new_data->t_hoff = new_header_len;

		/* 复制数据，并在需要时填空值位图*/
		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + new_header_len,
						new_data_len,
						&(new_data->t_infomask),
						((ttc.ttc_flags & TOAST_HAS_NULLS) != 0) ?
						new_data->t_bits : NULL);
	}
	else
		result_tuple = newtup;

	toast_tuple_cleanup(&ttc);

	return result_tuple;
}


/* ----------
 * toast_flatten_tuple -
 *
 *	把元组"展平"，使其中不含行外的已 TOAST 字段。
 *	（这并不消除压缩的或短头的 datum。）
 *
 *	注意：我们期望调用方已经检查过 HeapTupleHasExternal(tup)，
 *	因此不需要一条短路路径。
 * ----------
 */
HeapTuple
toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * 把元组拆成各个字段。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (!toast_isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * 组装重新配置后的元组。
	 */
	new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

	/*
	 * 务必复制元组的标识字段。我们也特意
	 * 复制可见性信息，以防有人在
	 * syscache 项里查看这些字段。
	 */
	new_tuple->t_self = tup->t_self;
	new_tuple->t_tableOid = tup->t_tableOid;

	new_tuple->t_data->t_choice = tup->t_data->t_choice;
	new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		tup->t_data->t_infomask & HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask2 |=
		tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

	/*
	 * 释放分配的临时值
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return new_tuple;
}


/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	把含有行外已 TOAST 字段的元组"展平"成一个 Datum。
 *	结果总是在当前内存上下文中 palloc 出来的。
 *
 *	我们有一条总规则：容器类型（行、数组、
 *	范围等）的 Datum 不得含有任何外部 TOAST 指针。若没有
 *	这条规则，为存储准备元组时就得查看每个 Datum 的内部，
 *	那会很贵，而且难以干净地扩展
 *	到新种类的容器类型。
 *
 *	然而，我们并不想说以 HeapTuples 表示的元组
 *	不能含有已 TOAST 的字段，因此应当在
 *	这样的 HeapTuple 被转换成 Datum 时调用本例程。
 *
 *	顺便也解压任何已压缩字段。这对
 *	正确性并非必要，但反映了一种预期：压缩
 *	施加于整个元组会比施加于单个
 *	字段更有效。不过我们还没有关心到要为此拆开
 *	再重组元组、只为去掉压缩字段的程度。
 *	因此调用方通常不会调用本函数，除非看到该元组
 *	至少有一个外部字段。
 *
 *	另一方面，行内短头 varlena 字段保持原样。
 *	若在这里把它们"解除 TOAST"，它们反正会在 heap_fill_tuple 里
 *	被改回短头格式。
 * ----------
 */
Datum
toast_flatten_tuple_to_datum(HeapTupleHeader tup,
							 uint32 tup_len,
							 TupleDesc tupleDesc)
{
	HeapTupleHeader new_data;
	int32		new_header_len;
	int32		new_data_len;
	int32		new_tuple_len;
	HeapTupleData tmptup;
	int			numAttrs = tupleDesc->natts;
	int			i;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/* 建立一个临时的 HeapTuple 控制结构*/
	tmptup.t_len = tup_len;
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tup;

	/*
	 * 把元组拆成各个字段。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (toast_isnull[i])
			has_nulls = true;
		else if (TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = detoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * 计算元组的新大小。
	 *
	 * 这应当与
	 * heap_toast_insert_or_update 中的重组代码一致。
	 */
	new_header_len = SizeofHeapTupleHeader;
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	new_header_len = MAXALIGN(new_header_len);
	new_data_len = heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);
	new_tuple_len = new_header_len + new_data_len;

	new_data = (HeapTupleHeader) palloc0(new_tuple_len);

	/*
	 * 复制现有元组头，但调整 natts 与 t_hoff。
	 */
	memcpy(new_data, tup, SizeofHeapTupleHeader);
	HeapTupleHeaderSetNatts(new_data, numAttrs);
	new_data->t_hoff = new_header_len;

	/* 正确设置组合 Datum 的头部字段*/
	HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);
	HeapTupleHeaderSetTypeId(new_data, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(new_data, tupleDesc->tdtypmod);

	/* 复制数据，并在需要时填空值位图*/
	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + new_header_len,
					new_data_len,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * 释放分配的临时值
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_build_flattened_tuple -
 *
 *	构造一个不含行外已 TOAST 字段的元组。
 *	（这并不消除压缩的或短头的 datum。）
 *
 *	这本质上就像 heap_form_tuple，只不过它会
 *	事先展开任何外部数据指针。
 *
 *	同时是否应当解压
 *	行内压缩的 datum，并不很清楚。目前我们不这么做。
 * ----------
 */
HeapTuple
toast_build_flattened_tuple(TupleDesc tupleDesc,
							Datum *values,
							bool *isnull)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			num_to_free;
	int			i;
	Datum		new_values[MaxTupleAttributeNumber];
	Pointer		freeable_values[MaxTupleAttributeNumber];

	/*
	 * 可以把调用方的 isnull 数组直接传给 heap_form_tuple，但
	 * values 数组我们可能需要修改。
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	memcpy(new_values, values, numAttrs * sizeof(Datum));

	num_to_free = 0;
	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * 查看非空的 varlena 属性
		 */
		if (!isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(new_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				new_values[i] = PointerGetDatum(new_value);
				freeable_values[num_to_free++] = (Pointer) new_value;
			}
		}
	}

	/*
	 * 组装重新配置后的元组。
	 */
	new_tuple = heap_form_tuple(tupleDesc, new_values, isnull);

	/*
	 * 释放分配的临时值
	 */
	for (i = 0; i < num_to_free; i++)
		pfree(freeable_values[i]);

	return new_tuple;
}

/*
 * 从堆表取出一段 TOAST 切片。
 *
 * toastrel 是要从中取出各个块的关系。
 * valueid 标识正在取出各个块的那个 TOAST 值。
 * attrsize 是该 TOAST 值的总大小。
 * sliceoffset 是 TOAST 值内开始取出的字节偏移。
 * slicelength 是要从该 TOAST 值取出的字节数。
 * result 是应当把结果写入的 varlena。
 */
void
heap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
					   int32 sliceoffset, int32 slicelength,
					   struct varlena *result)
{
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	TupleDesc	toasttupDesc = toastrel->rd_att;
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	int32		expectedchunk;
	int32		totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;
	int			startchunk;
	int			endchunk;
	int			num_indexes;
	int			validIndex;
	SnapshotData SnapshotToast;

	/* 寻找 TOAST 关系的有效索引*/
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + slicelength - 1) / TOAST_MAX_CHUNK_SIZE;
	Assert(endchunk <= totalchunks);

	/* 设置扫描键，以便从索引取出。*/
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * 若取出全部块，则不加额外条件。否则，对单个块用
	 * 等值条件，其余情况用范围条件。
	 */
	if (startchunk == 0 && endchunk == totalchunks - 1)
		nscankeys = 1;
	else if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2],
					(AttrNumber) 2,
					BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/* 准备扫描*/
	init_toast_snapshot(&SnapshotToast);
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   &SnapshotToast, nscankeys, toastkey);

	/*
	 * 按索引读取各个块
	 *
	 * 索引建在 (valueid, chunkidx) 上，因此它们会按序到来
	 */
	expectedchunk = startchunk;
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int32		curchunk;
		Pointer		chunk;
		bool		isnull;
		char	   *chunkdata;
		int32		chunksize;
		int32		expected_size;
		int32		chcpystrt;
		int32		chcpyend;

		/*
		 * 拿到一个块，取出序号和数据
		 */
		curchunk = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* 可能是 heap_form_tuple 自己造成的*/
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* 应当永远不会发生*/
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 valueid, RelationGetRelationName(toastrel));
			chunksize = 0;		/* 让编译器安静*/
			chunkdata = NULL;
		}

		/*
		 * 对已找到的数据做一些检查
		 */
		if (curchunk != expectedchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (expected %d) for toast value %u in %s",
									 curchunk, expectedchunk, valueid,
									 RelationGetRelationName(toastrel))));
		if (curchunk > endchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
									 curchunk,
									 startchunk, endchunk, valueid,
									 RelationGetRelationName(toastrel))));
		expected_size = curchunk < totalchunks - 1 ? TOAST_MAX_CHUNK_SIZE
			: attrsize - ((totalchunks - 1) * TOAST_MAX_CHUNK_SIZE);
		if (chunksize != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
									 chunksize, expected_size,
									 curchunk, totalchunks, valueid,
									 RelationGetRelationName(toastrel))));

		/*
		 * 把数据复制到结果中的正确位置
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (curchunk == startchunk)
			chcpystrt = sliceoffset % TOAST_MAX_CHUNK_SIZE;
		if (curchunk == endchunk)
			chcpyend = (sliceoffset + slicelength - 1) % TOAST_MAX_CHUNK_SIZE;

		memcpy(VARDATA(result) +
			   (curchunk * TOAST_MAX_CHUNK_SIZE - sliceoffset) + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		expectedchunk++;
	}

	/*
	 * 最后检查我们是否成功取出了该 datum
	 */
	if (expectedchunk != (endchunk + 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("missing chunk number %d for toast value %u in %s",
								 expectedchunk, valueid,
								 RelationGetRelationName(toastrel))));

	/* 结束扫描并关闭索引。*/
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
}
