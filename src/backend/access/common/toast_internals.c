/*-------------------------------------------------------------------------
 *
 * toast_internals.c
 *	  供 TOAST 系统内部使用的函数。
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_internals.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static bool toastrel_valueid_exists(Relation toastrel, Oid valueid);
static bool toastid_valueid_exists(Oid toastrelid, Oid valueid);

/* ----------
 * toast_compress_datum -
 *
 *	创建 varlena datum 的压缩版本
 *
 *	若失败（即压缩结果实际上比原来更大），
 *	则返回 NULL。若压缩数据会让元组变大，就不得使用
 *	这些压缩数据！
 *
 *	这里用 VAR{SIZE,DATA}_ANY，这样不必复制就能处理短 varlena。
 *	但不能处理外部或已压缩的 datum。
 * ----------
 */
Datum
toast_compress_datum(Datum value, char cmethod)
{
	struct varlena *tmp = NULL;
	int32		valsize;
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

	Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));
	Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));

	valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

	/* 若压缩方法无效，就用当前默认值 */
	if (!CompressionMethodIsValid(cmethod))
		cmethod = default_toast_compression;

	/*
	 * 按该压缩方法调用相应的压缩例程。
	 */
	switch (cmethod)
	{
		case TOAST_PGLZ_COMPRESSION:
			tmp = pglz_compress_datum((const struct varlena *) value);
			cmid = TOAST_PGLZ_COMPRESSION_ID;
			break;
		case TOAST_LZ4_COMPRESSION:
			tmp = lz4_compress_datum((const struct varlena *) value);
			cmid = TOAST_LZ4_COMPRESSION_ID;
			break;
		default:
			elog(ERROR, "invalid compression method %c", cmethod);
	}

	if (tmp == NULL)
		return PointerGetDatum(NULL);

	/*
	 * 即使压缩报告成功，我们也再核对实际大小，因为它
	 * 可能只在压缩数据里省下一个字节就满足了 --- 一旦计入
	 * 头和对齐填充，这可能变成净亏损。最坏情况下，压缩格式
	 * 可能需要三个填充字节（外加头，VARSIZE(tmp) 已含该头），
	 * 而未压缩格式在值足够短时只会占用一个头字节，
	 * 并且没有任何填充。因此我们坚持
	 * 节省必须超过 2 字节，才能确保有收益。
	 * 这样才算真正有收益。
	 */
	if (VARSIZE(tmp) < valsize - 2)
	{
		/* 压缩成功 */
		Assert(cmid != TOAST_INVALID_COMPRESSION_ID);
		TOAST_COMPRESS_SET_SIZE_AND_COMPRESS_METHOD(tmp, valsize, cmid);
		return PointerGetDatum(tmp);
	}
	else
	{
		/* 不可压缩的数据 */
		pfree(tmp);
		return PointerGetDatum(NULL);
	}
}

/* ----------
 * toast_save_datum -
 *
 *	把单个 datum 存入次级关系，并返回
 *	一个指向它的 Datum 引用。
 *
 * rel：正在处理的主关系（不是 toast 关系！）
 * value：要推入 toast 存储的 datum
 * oldexternal：若非 NULL，是先前代表该 datum 的 toast 指针
 * options：要传给 heap_insert()、用于 toast 行的选项
 * ----------
 */
Datum
toast_save_datum(Relation rel, Datum value,
				 struct varlena *oldexternal, int options)
{
	Relation	toastrel;
	Relation   *toastidxs;
	HeapTuple	toasttup;
	TupleDesc	toasttupDesc;
	Datum		t_values[3];
	bool		t_isnull[3];
	CommandId	mycid = GetCurrentCommandId(true);
	struct varlena *result;
	struct varatt_external toast_pointer;
	union
	{
		struct varlena hdr;
		/* 这是为了让联合体大到能放下一个块： */
		char		data[TOAST_MAX_CHUNK_SIZE + VARHDRSZ];
		/* 确保联合体对齐得足够好： */
		int32		align_it;
	}			chunk_data = {0};	/* 消除编译器告警 */
	int32		chunk_size;
	int32		chunk_seq = 0;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);
	int			num_indexes;
	int			validIndex;

	Assert(!VARATT_IS_EXTERNAL(value));

	/*
	 * 打开 toast 关系及其索引。可以用该索引检查我们分配给
	 * 已 TOAST 项的 OID 是否唯一，尽管除 OID 之外
	 * 它还有其他列。
	 */
	toastrel = table_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toasttupDesc = toastrel->rd_att;

	/* 打开全部 toast 索引并找出有效的那一个 */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * 取得数据指针和长度，并计算 va_rawsize 与 va_extinfo。
	 *
	 * va_rawsize 是等价的完全未压缩 datum 的大小，
	 * 因此必须针对短头做调整。
	 *
	 * va_extinfo 存放 toast 记录里数据有效载荷的实际大小，
	 * 并且在数据已压缩时，用前 2 位存放
	 * 压缩方法。
	 */
	if (VARATT_IS_SHORT(dval))
	{
		data_p = VARDATA_SHORT(dval);
		data_todo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		toast_pointer.va_rawsize = data_todo + VARHDRSZ;	/* 当作不是短头 */
		toast_pointer.va_extinfo = data_todo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		/* 压缩 datum 里的 rawsize 只是有效载荷大小 */
		toast_pointer.va_rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(dval) + VARHDRSZ;

		/* 设置外部大小和压缩方法 */
		VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, data_todo,
													 VARDATA_COMPRESSED_GET_COMPRESS_METHOD(dval));
		/* 断言这些数字看起来像是已压缩的 */
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
	}
	else
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(dval);
		toast_pointer.va_extinfo = data_todo;
	}

	/*
	 * 把正确的表 OID 写入结果 TOAST 指针。
	 *
	 * 通常这就是目标 toast 表的实际 OID，但在诸如 CLUSTER
	 * 这样的表重写操作期间，必须改为写入该表真正的永久
	 * toast 表的 OID。若必须替换这样的 OID，
	 * 就会设置 rd_toastoid。
	 */
	if (OidIsValid(rel->rd_toastoid))
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = RelationGetRelid(toastrel);

	/*
	 * 为这个 toast 值选择一个用作值 ID 的 OID。
	 *
	 * 通常只在 toast 表中选一个未使用的 OID。但在
	 * 保留已有 toast 表 OID 的表重写期间，我们也要保留
	 * toast 值 OIDs。因此，若已设置 rd_toastoid，且先前有
	 * 来自同一张 toast 表的外部值，就复用它的值 ID。
	 * 若没有先前的外部值（这是角落情形，但若该表的
	 * attstorage 选项已被修改就可能发生），就必须选一个
	 * 与新 toast 值 OID 或已有 toast 值 OID 都不冲突的
	 * 值 ID。
	 */
	if (!OidIsValid(rel->rd_toastoid))
	{
		/* 普通情形：选一个未使用的 OID */
		toast_pointer.va_valueid =
			GetNewOidWithIndex(toastrel,
							   RelationGetRelid(toastidxs[validIndex]),
							   (AttrNumber) 1);
	}
	else
	{
		/* 重写情形：检查该值是否在旧 toast 表中 */
		toast_pointer.va_valueid = InvalidOid;
		if (oldexternal != NULL)
		{
			struct varatt_external old_toast_pointer;

			Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal));
			/* 必须复制才能访问已对齐的字段 */
			VARATT_EXTERNAL_GET_POINTER(old_toast_pointer, oldexternal);
			if (old_toast_pointer.va_toastrelid == rel->rd_toastoid)
			{
				/* 该值来自旧 toast 表；复用它的 OID */
				toast_pointer.va_valueid = old_toast_pointer.va_valueid;

				/*
				 * There is a corner case here: the table rewrite might have
				 * to copy both live and recently-dead versions of a row, and
				 * those versions could easily reference the same toast value.
				 * When we copy the second or later version of such a row,
				 * reusing the OID will mean we select an OID that's already
				 * in the new toast table.  Check for that, and if so, just
				 * fall through without writing the data again.
				 *
				 * While annoying and ugly-looking, this is a good thing
				 * because it ensures that we wind up with only one copy of
				 * the toast value when there is only one copy in the old
				 * toast table.  Before we detected this case, we'd have made
				 * multiple copies, wasting space; and what's worse, the
				 * copies belonging to already-deleted heap tuples would not
				 * be reclaimed by VACUUM.
				 */
				if (toastrel_valueid_exists(toastrel,
											toast_pointer.va_valueid))
				{
					/* 匹配，于是短路下面的数据存储循环 */
					data_todo = 0;
				}
			}
		}
		if (toast_pointer.va_valueid == InvalidOid)
		{
			/*
			 * 新值；必须选一个在旧 toast 表和新 toast 表中
			 * 都不冲突的 OID
			 */
			do
			{
				toast_pointer.va_valueid =
					GetNewOidWithIndex(toastrel,
									   RelationGetRelid(toastidxs[validIndex]),
									   (AttrNumber) 1);
			} while (toastid_valueid_exists(rel->rd_toastoid,
											toast_pointer.va_valueid));
		}
	}

	/*
	 * 初始化元组数据中的常量部分
	 */
	t_values[0] = ObjectIdGetDatum(toast_pointer.va_valueid);
	t_values[2] = PointerGetDatum(&chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * 把该项拆成多个块
	 */
	while (data_todo > 0)
	{
		int			i;

		CHECK_FOR_INTERRUPTS();

		/*
		 * 计算这个块的大小
		 */
		chunk_size = Min(TOAST_MAX_CHUNK_SIZE, data_todo);

		/*
		 * 构造元组并存储它
		 */
		t_values[1] = Int32GetDatum(chunk_seq++);
		SET_VARSIZE(&chunk_data, chunk_size + VARHDRSZ);
		memcpy(VARDATA(&chunk_data), data_p, chunk_size);
		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		heap_insert(toastrel, toasttup, mycid, options, NULL);

		/*
		 * 创建索引项。这里有点偷懒，没有使用
		 * FormIndexDatum：这依赖于一项知识，即对这些索引而言，
		 * 索引列与表的初始列相同。
		 * 我们也不提供 IndexInfo，同样是偷懒：目前没问题，
		 * 因为 btree 并不需要它，但将来某一天
		 * 也许得更老实一些。
		 *
		 * 还要注意，TOAST 表上最好不要有任何用户创建的索引，
		 * 因为我们不会去更新其他任何东西。
		 */
		for (i = 0; i < num_indexes; i++)
		{
			/* 只有标记为就绪的索引关系才能更新 */
			if (toastidxs[i]->rd_index->indisready)
				index_insert(toastidxs[i], t_values, t_isnull,
							 &(toasttup->t_self),
							 toastrel,
							 toastidxs[i]->rd_index->indisunique ?
							 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
							 false, NULL);
		}

		/*
		 * 释放内存
		 */
		heap_freetuple(toasttup);

		/*
		 * 前进到下一块
		 */
		data_todo -= chunk_size;
		data_p += chunk_size;
	}

	/*
	 * 完成 --- 关闭 toast 关系及其索引，但把锁保持到提交，
	 * 这样直接在该 toast 关系上并发进行的重建索引就能
	 * 等待本事务。
	 */
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);

	/*
	 * 创建我们将返回的 TOAST 指针值
	 */
	result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));

	return PointerGetDatum(result);
}

/* ----------
 * toast_delete_datum -
 *
 *	删除单个外部存储的值。
 * ----------
 */
void
toast_delete_datum(Relation rel, Datum value, bool is_speculative)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	toasttup;
	int			num_indexes;
	int			validIndex;
	SnapshotData SnapshotToast;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/* 必须复制才能访问已对齐的字段 */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * 打开 toast 关系及其索引
	 */
	toastrel = table_open(toast_pointer.va_toastrelid, RowExclusiveLock);

	/* 取得处理所用的有效关系 */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * 设置扫描键，以查找 va_valueid 匹配的块
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * 找出所有块。（我们其实不在乎是否按顺序看到它们，
	 * 但既然已经锁住了索引，不妨使用
	 * systable_beginscan_ordered。）
	 */
	init_toast_snapshot(&SnapshotToast);
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   &SnapshotToast, 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * 有一个块，删掉它
		 */
		if (is_speculative)
			heap_abort_speculative(toastrel, &toasttup->t_self);
		else
			simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * 结束扫描并关闭各关系，但把锁保持到提交，以便直接在该
	 * toast 关系上并发进行的重建索引能够等待
	 * 本事务。
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);
}

/* ----------
 * toastrel_valueid_exists -
 *
 *	测试给定 ID 的 toast 值是否存在于该 toast 关系中。
 *	为安全起见，只要存在该 ID 的活行或死行，就认为该值存在；
 *	参见 GetNewOidWithIndex() 的说明。
 * ----------
 */
static bool
toastrel_valueid_exists(Relation toastrel, Oid valueid)
{
	bool		result = false;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	int			num_indexes;
	int			validIndex;
	Relation   *toastidxs;

	/* 取得一个有效的索引关系 */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * 设置扫描键，以查找 va_valueid 匹配的块
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * 有没有这样的块？
	 */
	toastscan = systable_beginscan(toastrel,
								   RelationGetRelid(toastidxs[validIndex]),
								   true, SnapshotAny, 1, &toastkey);

	if (systable_getnext(toastscan) != NULL)
		result = true;

	systable_endscan(toastscan);

	/* 做清理 */
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);

	return result;
}

/* ----------
 * toastid_valueid_exists -
 *
 *	同上，但根据 toast 关系的 OID 工作，而不是已打开的关系
 * ----------
 */
static bool
toastid_valueid_exists(Oid toastrelid, Oid valueid)
{
	bool		result;
	Relation	toastrel;

	toastrel = table_open(toastrelid, AccessShareLock);

	result = toastrel_valueid_exists(toastrel, valueid);

	table_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_get_valid_index
 *
 *	取得与给定 toast 关系相关联的有效索引的 OID。一张 toast
 *	关系在同一时刻只能有一个有效索引。
 */
Oid
toast_get_valid_index(Oid toastoid, LOCKMODE lock)
{
	int			num_indexes;
	int			validIndex;
	Oid			validIndexOid;
	Relation   *toastidxs;
	Relation	toastrel;

	/* 打开 toast 关系 */
	toastrel = table_open(toastoid, lock);

	/* 寻找该 toast 关系的有效索引 */
	validIndex = toast_open_indexes(toastrel,
									lock,
									&toastidxs,
									&num_indexes);
	validIndexOid = RelationGetRelid(toastidxs[validIndex]);

	/* 关闭 toast 关系及其全部索引 */
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);

	return validIndexOid;
}

/* ----------
 * toast_open_indexes
 *
 *	取得与给定 toast 关系相关联的索引数组，同时返回
 *	该 toast 关系在此数组中所使用的有效索引的位置。
 *	调用本函数的调用方有责任关闭这些索引，
 *	并释放它们。
 */
int
toast_open_indexes(Relation toastrel,
				   LOCKMODE lock,
				   Relation **toastidxs,
				   int *num_indexes)
{
	int			i = 0;
	int			res = 0;
	bool		found = false;
	List	   *indexlist;
	ListCell   *lc;

	/* 取得 toast 关系的索引列表 */
	indexlist = RelationGetIndexList(toastrel);
	Assert(indexlist != NIL);

	*num_indexes = list_length(indexlist);

	/* 打开全部索引关系 */
	*toastidxs = (Relation *) palloc(*num_indexes * sizeof(Relation));
	foreach(lc, indexlist)
		(*toastidxs)[i++] = index_open(lfirst_oid(lc), lock);

	/* 取列表中第一个有效索引 */
	for (i = 0; i < *num_indexes; i++)
	{
		Relation	toastidx = (*toastidxs)[i];

		if (toastidx->rd_index->indisvalid)
		{
			res = i;
			found = true;
			break;
		}
	}

	/*
	 * 释放索引列表。关系已经打开，并且已经找到有效索引，
	 * 不再需要这份列表。
	 */
	list_free(indexlist);

	/*
	 * toast 关系应当有一个有效索引，若什么都没有，
	 * 就说明出了问题。
	 */
	if (!found)
		elog(ERROR, "no valid index found for toast relation with Oid %u",
			 RelationGetRelid(toastrel));

	return res;
}

/* ----------
 * toast_close_indexes
 *
 *	关闭 toast 关系的索引数组并释放它。应当对先前用
 *	toast_open_indexes 打开的那组索引调用本函数。
 */
void
toast_close_indexes(Relation *toastidxs, int num_indexes, LOCKMODE lock)
{
	int			i;

	/* 关闭关系并做清理 */
	for (i = 0; i < num_indexes; i++)
		index_close(toastidxs[i], lock);
	pfree(toastidxs);
}

/* ----------
 * init_toast_snapshot
 *
 *	初始化一份合适的 TOAST 快照。必须用一份 MVCC 快照来
 *	初始化 TOAST 快照；既然不知道该用哪一份，就用最旧的。
 *	这样做是安全的：最坏会得到一个本可避免的 "snapshot
 *	too old"（快照过旧）错误。
 */
void
init_toast_snapshot(Snapshot toast_snapshot)
{
	Snapshot	snapshot = GetOldestSnapshot();

	/*
	 * 若会话没有活动快照，GetOldestSnapshot 就返回 NULL。
	 * 例如，一个过程把已 TOAST 的值取进局部变量，
	 * 提交之后又试图还原该值，就会碰到这种情况。
	 * 这样写并不安全，因为一旦提交，就没有任何东西
	 * 能阻止 toast 数据被删除。还原*必须*发生在
	 * 最初取出 toast 指针的同一个事务中。因此，
	 * 与其试图把问题补上，不如直接抛出错误。（这算不上
	 * 多少保护，因为在许多情形下该过程已经建立了新的
	 * 事务快照，使我们发现不了这个问题。
	 * 但总比没有好，而且我们当然不该再花代码
	 * 去把问题掩盖得更深。）
	 */
	if (snapshot == NULL)
		elog(ERROR, "cannot fetch toast data without an active snapshot");

	/*
	 * 即使未注册或未活动，GetOldestSnapshot() 也可能返回
	 * 系统目录快照。这很容易掩盖没有设好快照的缺陷 ---
	 * 大多数时候都存在一份有效的系统目录快照。
	 * 因此再额外坚持：当前快照必须已注册，
	 * 或者处于活动状态。
	 */
	Assert(HaveRegisteredOrActiveSnapshot());

	InitToastSnapshot(*toast_snapshot, snapshot->lsn, snapshot->whenTaken);
}
