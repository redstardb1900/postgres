/*-------------------------------------------------------------------------
 *
 * inv_api.c
 *	  操作 inversion 文件系统大对象的例程。本文件
 *	  包含用户级大对象应用接口例程。
 *
 *
 * 注意：我们借助 C 结构体声明访问 pg_largeobject.data。
 * 这样做是安全的，因为它紧跟在 int4 字段 pageno 之后，
 * 因此 data 字段始终按 4 字节对齐，即便它
 * 处于短的 1 字节头格式。由于它很
 * 可能处于压缩或短格式，我们必须对它做 detoast。还要检查
 * NULLs，因为 initdb 会把 loid 和 pageno 标为 NOT NULL，但不把 data 标为 NOT NULL。
 *
 * 注意：这些例程中有许多会在 CurrentMemoryContext 里泄漏内存，正如
 * 大多数后端代码那样。我们预期 CurrentMemoryContext 将
 * 是短生命周期的内存上下文。必须跨函数调用保持的数据
 * 要么放在 CacheMemoryContext（Relation 结构体）中，要么放在
 * 交给 inv_open 的内存上下文（LargeObjectDesc 结构体）中。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/large_object/inv_api.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/large_object.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


/*
 * GUC：抑制大对象权限检查的向后兼容标志
 */
bool		lo_compat_privileges;

/*
 * 对 pg_largeobject 及其索引的全部访问都共用一个 Relation
 * 引用，这样每个事务只需打开 pg_relation 一次。
 * 为避免第一次这样的引用出现在
 * 子事务内部时出问题，我们采用一种有点凑合的权宜之计，把
 * 该 Relation 引用的所有权交给 TopTransactionResourceOwner。
 */
static Relation lo_heap_r = NULL;
static Relation lo_index_r = NULL;


/*
 * 打开 pg_largeobject 及其索引（若当前事务中尚未打开）
 */
static void
open_lo_relation(void)
{
	ResourceOwner currentOwner;

	if (lo_heap_r && lo_index_r)
		return;					/* 当前事务中已经打开 */

	/* 让顶层事务拥有这些关系引用 */
	currentOwner = CurrentResourceOwner;
	CurrentResourceOwner = TopTransactionResourceOwner;

	/* 使用 RowExclusiveLock，因为我们可能读也可能写 */
	if (lo_heap_r == NULL)
		lo_heap_r = table_open(LargeObjectRelationId, RowExclusiveLock);
	if (lo_index_r == NULL)
		lo_index_r = index_open(LargeObjectLOidPNIndexId, RowExclusiveLock);

	CurrentResourceOwner = currentOwner;
}

/*
 * 在主事务结束时清理
 */
void
close_lo_relation(bool isCommit)
{
	if (lo_heap_r || lo_index_r)
	{
		/*
		 * 只在提交时才费心关闭；否则中止清理会负责
		 * 处理
		 */
		if (isCommit)
		{
			ResourceOwner currentOwner;

			currentOwner = CurrentResourceOwner;
			CurrentResourceOwner = TopTransactionResourceOwner;

			if (lo_index_r)
				index_close(lo_index_r, NoLock);
			if (lo_heap_r)
				table_close(lo_heap_r, NoLock);

			CurrentResourceOwner = currentOwner;
		}
		lo_heap_r = NULL;
		lo_index_r = NULL;
	}
}


/*
 * 与 pg_largeobject.c 的 LargeObjectExists() 相同，只是可以指定
 * 读取时使用的快照。
 */
static bool
myLargeObjectExists(Oid loid, Snapshot snapshot)
{
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;
	bool		retval = false;

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_metadata_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_lo_meta = table_open(LargeObjectMetadataRelationId,
							AccessShareLock);

	sd = systable_beginscan(pg_lo_meta,
							LargeObjectMetadataOidIndexId, true,
							snapshot, 1, skey);

	tuple = systable_getnext(sd);
	if (HeapTupleIsValid(tuple))
		retval = true;

	systable_endscan(sd);

	table_close(pg_lo_meta, AccessShareLock);

	return retval;
}


/*
 * 从 pg_largeobject 元组中取出 data 字段，必要时做 detoast，
 * 并核实长度是否合理。返回数据指针（bytea *）、
 * 数据长度，以及是否应当 pfree 该数据指针。
 */
static void
getdatafield(Form_pg_largeobject tuple,
			 bytea **pdatafield,
			 int *plen,
			 bool *pfreeit)
{
	bytea	   *datafield;
	int			len;
	bool		freeit;

	datafield = &(tuple->data); /* 见文件开头的说明 */
	freeit = false;
	if (VARATT_IS_EXTENDED(datafield))
	{
		datafield = (bytea *)
			detoast_attr((struct varlena *) datafield);
		freeit = true;
	}
	len = VARSIZE(datafield) - VARHDRSZ;
	if (len < 0 || len > LOBLKSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("pg_largeobject entry for OID %u, page %d has invalid data field size %d",
						tuple->loid, tuple->pageno, len)));
	*pdatafield = datafield;
	*plen = len;
	*pfreeit = freeit;
}


/*
 *	inv_create -- 创建一个新的大对象
 *
 *	参数：
 *	  lobjId - 新大对象使用的 OID，或用 InvalidOid 表示自行选取
 *
 *	返回：
 *	  新对象的 OID
 *
 * 若 lobjId 不是 InvalidOid，而该 OID 已经
 * 被占用，则报错。
 */
Oid
inv_create(Oid lobjId)
{
	Oid			lobjId_new;

	/*
	 * 创建一个带空数据页的新大对象
	 */
	lobjId_new = LargeObjectCreate(lobjId);

	/*
	 * 对大对象所有者的依赖
	 *
	 * 注意：出于向后兼容，大对象依赖用 classId
	 * LargeObjectRelationId 来记录。改用
	 * LargeObjectMetadataRelationId 则会简化后端的
	 * 工作，但会让 pg_dump 更复杂，并可能破坏其他客户端。
	 */
	recordDependencyOnOwner(LargeObjectRelationId,
							lobjId_new, GetUserId());

	/* 新大对象的创建后钩子 */
	InvokeObjectPostCreateHook(LargeObjectRelationId, lobjId_new, 0);

	/*
	 * 推进命令计数器，使新元组对后续操作可见。
	 */
	CommandCounterIncrement();

	return lobjId_new;
}

/*
 *	inv_open -- 访问一个已有的大对象。
 *
 * 返回一个已适当填好的大对象描述符。
 * 描述符及其附属数据在指定的
 * 内存上下文中分配，该上下文的寿命必须足以满足调用者的
 * 用途。若返回的描述符关联了快照，
 * 调用者必须保证该快照也活得足够久，
 * 例如调用 RegisterSnapshotOnOwner
 */
LargeObjectDesc *
inv_open(Oid lobjId, int flags, MemoryContext mcxt)
{
	LargeObjectDesc *retval;
	Snapshot	snapshot = NULL;
	int			descflags = 0;

	/*
	 * 历史上，(INV_WRITE) 与 (INV_WRITE
	 * | INV_READ) 不作区分；两种情况下调用者都可以读取
	 * 该大对象描述符。
	 */
	if (flags & INV_WRITE)
		descflags |= IFS_WRLOCK | IFS_RDLOCK;
	if (flags & INV_READ)
		descflags |= IFS_RDLOCK;

	if (descflags == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid flags for opening a large object: %d",
						flags)));

	/* 获取快照。若请求写入，则使用瞬时快照。 */
	if (descflags & IFS_WRLOCK)
		snapshot = NULL;
	else
		snapshot = GetActiveSnapshot();

	/* 这里不能用 LargeObjectExists，因为需要指定快照 */
	if (!myLargeObjectExists(lobjId, snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobjId)));

	/* 进行权限检查，同样要指定快照 */
	if ((descflags & IFS_RDLOCK) != 0)
	{
		if (!lo_compat_privileges &&
			pg_largeobject_aclcheck_snapshot(lobjId,
											 GetUserId(),
											 ACL_SELECT,
											 snapshot) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for large object %u",
							lobjId)));
	}
	if ((descflags & IFS_WRLOCK) != 0)
	{
		if (!lo_compat_privileges &&
			pg_largeobject_aclcheck_snapshot(lobjId,
											 GetUserId(),
											 ACL_UPDATE,
											 snapshot) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied for large object %u",
							lobjId)));
	}

	/* 可以创建描述符了 */
	retval = (LargeObjectDesc *) MemoryContextAlloc(mcxt,
													sizeof(LargeObjectDesc));
	retval->id = lobjId;
	retval->offset = 0;
	retval->flags = descflags;

	/* 由调用者按需设置，本文件中的函数不使用 */
	retval->subid = InvalidSubTransactionId;

	/*
	 * 快照（若有）只是当前活动快照。调用者
	 * 会在需要时把它换成寿命更长的副本。
	 */
	retval->snapshot = snapshot;

	return retval;
}

/*
 * 关闭先前由 inv_open() 创建的大对象描述符，并
 * 释放它占用的长期内存。
 */
void
inv_close(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));
	pfree(obj_desc);
}

/*
 * 销毁一个已有的大对象（不要与描述符混淆！）
 *
 * 注意：我们预期调用者已经做过任何必要的权限检查。
 */
int
inv_drop(Oid lobjId)
{
	ObjectAddress object;

	/*
	 * 删除该大对象上的全部注释和依赖
	 */
	object.classId = LargeObjectRelationId;
	object.objectId = lobjId;
	object.objectSubId = 0;
	performDeletion(&object, DROP_CASCADE, 0);

	/*
	 * 推进命令计数器，使本事务中后续的
	 * 大对象操作能看到元组删除。
	 */
	CommandCounterIncrement();

	/* 出于历史原因，成功时我们总是返回 1。 */
	return 1;
}

/*
 * 确定大对象的大小
 *
 * 注意：大对象可以含有空洞，就像 Unix 文件一样。我们实际返回
 * 最后一个字节的偏移 + 1。
 */
static uint64
inv_getsize(LargeObjectDesc *obj_desc)
{
	uint64		lastbyte = 0;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 1, skey);

	/*
	 * 因为 pg_largeobject 索引同时包含 loid 和 pageno，但我们
	 * 只约束 loid，反向扫描会按 pageno 逆序访问该
	 * 大对象的全部页。因此只需检查
	 * 第一条有效元组（== 最后一个有效页）。
	 */
	tuple = systable_getnext_ordered(sd, BackwardScanDirection);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		int			len;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* 以防万一 */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);
		getdatafield(data, &datafield, &len, &pfreeit);
		lastbyte = (uint64) data->pageno * LOBLKSIZE + len;
		if (pfreeit)
			pfree(datafield);
	}

	systable_endscan_ordered(sd);

	return lastbyte;
}

int64
inv_seek(LargeObjectDesc *obj_desc, int64 offset, int whence)
{
	int64		newoffset;

	Assert(PointerIsValid(obj_desc));

	/*
	 * 只要有读或写权限就允许 seek/tell，因此这里
	 * 不必做权限检查。
	 */

	/*
	 * 注意：加法可能溢出，但由于我们会拒绝
	 * 负的结果，不必为此再做额外检查。
	 */
	switch (whence)
	{
		case SEEK_SET:
			newoffset = offset;
			break;
		case SEEK_CUR:
			newoffset = obj_desc->offset + offset;
			break;
		case SEEK_END:
			newoffset = inv_getsize(obj_desc) + offset;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid whence setting: %d", whence)));
			newoffset = 0;		/* 让编译器安静 */
			break;
	}

	/*
	 * 这里用 errmsg_internal，因为不想在可翻译字符串里暴露 INT64_FORMAT；
	 * 做得更好并不值得费这个劲
	 */
	if (newoffset < 0 || newoffset > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg_internal("invalid large object seek target: " INT64_FORMAT,
								 newoffset)));

	obj_desc->offset = newoffset;
	return newoffset;
}

int64
inv_tell(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	/*
	 * 只要有读或写权限就允许 seek/tell，因此这里
	 * 不必做权限检查。
	 */

	return obj_desc->offset;
}

int
inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes)
{
	int			nread = 0;
	int64		n;
	int64		off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	uint64		pageoff;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	if ((obj_desc->flags & IFS_RDLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	if (nbytes <= 0)
		return 0;

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	while ((tuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* 以防万一 */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);

		/*
		 * 我们预期索引扫描会按顺序给出各页。不过，
		 * 若大对象含有未写过的空洞，就可能缺少页。我们
		 * 希望缺少的区段读出来是零。
		 */
		pageoff = ((uint64) data->pageno) * LOBLKSIZE;
		if (pageoff > obj_desc->offset)
		{
			n = pageoff - obj_desc->offset;
			n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
			MemSet(buf + nread, 0, n);
			nread += n;
			obj_desc->offset += n;
		}

		if (nread < nbytes)
		{
			Assert(obj_desc->offset >= pageoff);
			off = (int) (obj_desc->offset - pageoff);
			Assert(off >= 0 && off < LOBLKSIZE);

			getdatafield(data, &datafield, &len, &pfreeit);
			if (len > off)
			{
				n = len - off;
				n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
				memcpy(buf + nread, VARDATA(datafield) + off, n);
				nread += n;
				obj_desc->offset += n;
			}
			if (pfreeit)
				pfree(datafield);
		}

		if (nread >= nbytes)
			break;
	}

	systable_endscan_ordered(sd);

	return nread;
}

int
inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes)
{
	int			nwritten = 0;
	int			n;
	int			off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	bool		neednextpage;
	bytea	   *datafield;
	bool		pfreeit;
	union
	{
		bytea		hdr;
		/* 这是为了让联合体大到能放下一个大对象数据块： */
		char		data[LOBLKSIZE + VARHDRSZ];
		/* 保证联合体对齐得足够好： */
		int32		align_it;
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	/* 强制要求可写，否则快照很可能是错的 */
	if ((obj_desc->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	if (nbytes <= 0)
		return 0;

	/* 这次加法不会溢出，因为 nbytes 只是 int32 */
	if ((nbytes + obj_desc->offset) > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid large object write request size: %d",
						nbytes)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	oldtuple = NULL;
	olddata = NULL;
	neednextpage = true;

	while (nwritten < nbytes)
	{
		/*
		 * 若可能，取该大对象的下一个已有页。我们预期
		 * 索引扫描会按顺序给出这些页——但可能有空洞。
		 */
		if (neednextpage)
		{
			if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
			{
				if (HeapTupleHasNulls(oldtuple))	/* 以防万一 */
					elog(ERROR, "null field found in pg_largeobject");
				olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
				Assert(olddata->pageno >= pageno);
			}
			neednextpage = false;
		}

		/*
		 * 若已有一个先前存在的页，看它是我们要
		 * 写入的那一页，还是更后面的一页。
		 */
		if (olddata != NULL && olddata->pageno == pageno)
		{
			/*
			 * 用新数据更新一个已有页。
			 *
			 * 首先，把旧数据装入 workbuf
			 */
			getdatafield(olddata, &datafield, &len, &pfreeit);
			memcpy(workb, VARDATA(datafield), len);
			if (pfreeit)
				pfree(datafield);

			/*
			 * 填上任何空洞
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > len)
				MemSet(workb + len, 0, off - len);

			/*
			 * 插入新数据中适当的部分
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			off += n;
			/* 计算新页的有效长度 */
			len = (len >= off) ? len : off;
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * 构造并插入更新后的元组
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			memset(replace, false, sizeof(replace));
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			replace[Anum_pg_largeobject_data - 1] = true;
			newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
									   values, nulls, replace);
			CatalogTupleUpdateWithInfo(lo_heap_r, &newtup->t_self, newtup,
									   indstate);
			heap_freetuple(newtup);

			/*
			 * 这个旧页已经处理完。
			 */
			oldtuple = NULL;
			olddata = NULL;
			neednextpage = true;
		}
		else
		{
			/*
			 * 写入一个全新的页。
			 *
			 * 首先，填上任何空洞
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > 0)
				MemSet(workb, 0, off);

			/*
			 * 插入新数据中适当的部分
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			/* 计算新页的有效长度 */
			len = off + n;
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * 构造并插入更新后的元组
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
			values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
			CatalogTupleInsertWithInfo(lo_heap_r, newtup, indstate);
			heap_freetuple(newtup);
		}
		pageno++;
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * 推进命令计数器，使本事务中后续的
	 * 大对象操作能看到我的元组更新。
	 */
	CommandCounterIncrement();

	return nwritten;
}

void
inv_truncate(LargeObjectDesc *obj_desc, int64 len)
{
	int32		pageno = (int32) (len / LOBLKSIZE);
	int32		off;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	union
	{
		bytea		hdr;
		/* 这是为了让联合体大到能放下一个大对象数据块： */
		char		data[LOBLKSIZE + VARHDRSZ];
		/* 保证联合体对齐得足够好： */
		int32		align_it;
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));

	/* 强制要求可写，否则快照很可能是错的 */
	if ((obj_desc->flags & IFS_WRLOCK) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for large object %u",
						obj_desc->id)));

	/*
	 * 这里用 errmsg_internal，因为不想在可翻译字符串里暴露 INT64_FORMAT；
	 * 做得更好并不值得费这个劲
	 */
	if (len < 0 || len > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg_internal("invalid large object truncation target: " INT64_FORMAT,
								 len)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	/*
	 * 准备查找具有指定 loid 且 pageno >= 目标值的全部页
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	/*
	 * 若可能，取截断点所在的页。该截断点
	 * 可能超出大对象末尾，或落在空洞中。
	 */
	olddata = NULL;
	if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		if (HeapTupleHasNulls(oldtuple))	/* 以防万一 */
			elog(ERROR, "null field found in pg_largeobject");
		olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
		Assert(olddata->pageno >= pageno);
	}

	/*
	 * 若找到了截断点所在的页，需要截断
	 * 其中的数据。否则若我们处于空洞中，需要创建一页来
	 * 标记数据的末尾。
	 */
	if (olddata != NULL && olddata->pageno == pageno)
	{
		/* 首先，把旧数据装入 workbuf */
		bytea	   *datafield;
		int			pagelen;
		bool		pfreeit;

		getdatafield(olddata, &datafield, &pagelen, &pfreeit);
		memcpy(workb, VARDATA(datafield), pagelen);
		if (pfreeit)
			pfree(datafield);

		/*
		 * 填上任何空洞
		 */
		off = len % LOBLKSIZE;
		if (off > pagelen)
			MemSet(workb + pagelen, 0, off - pagelen);

		/* 计算新页的长度 */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * 构造并插入更新后的元组
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replace, false, sizeof(replace));
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		replace[Anum_pg_largeobject_data - 1] = true;
		newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
								   values, nulls, replace);
		CatalogTupleUpdateWithInfo(lo_heap_r, &newtup->t_self, newtup,
								   indstate);
		heap_freetuple(newtup);
	}
	else
	{
		/*
		 * 若找到的第一页在截断点之后，我们处于
		 * 一个即将填上的空洞中，但仍需删除后面那一页，
		 * 因为下面的循环不会再访问它。
		 */
		if (olddata != NULL)
		{
			Assert(olddata->pageno > pageno);
			CatalogTupleDelete(lo_heap_r, &oldtuple->t_self);
		}

		/*
		 * 写入一个全新的页。
		 *
		 * 把空洞填到截断点
		 */
		off = len % LOBLKSIZE;
		if (off > 0)
			MemSet(workb, 0, off);

		/* 计算新页的长度 */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * 构造并插入新元组
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
		values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
		CatalogTupleInsertWithInfo(lo_heap_r, newtup, indstate);
		heap_freetuple(newtup);
	}

	/*
	 * 删除截断点之后的任何页。若最初的搜索
	 * 没有找到页，那当然就没有更多事情要做。
	 */
	if (olddata != NULL)
	{
		while ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
		{
			CatalogTupleDelete(lo_heap_r, &oldtuple->t_self);
		}
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * 推进命令计数器，使本事务中后续的
	 * 大对象操作能看到元组更新。
	 */
	CommandCounterIncrement();
}
