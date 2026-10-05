/*-------------------------------------------------------------------------
 *
 * sharedfileset.c
 *	  共享临时文件的管理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/sharedfileset.c
 *
 * SharedFileSets 提供一个临时名字空间（可以把它想成目录），使
 * 文件能按名字被找到，并提供共享所有权语义，使
 * 共享文件一直存活到最后一个使用者脱离。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/sharedfileset.h"
#include "utils/builtins.h"

static void SharedFileSetOnDetach(dsm_segment *segment, Datum datum);

/*
 * 为其他后端也能打开的临时文件初始化一块空间。
 * 其他后端在访问之前必须挂接上来。把这个
 * SharedFileSet 与 seg 关联。所含文件会在
 * 最后一个后端脱离时被删除。
 *
 * 在内部，这个集合是一个或多个目录，最终
 * 会被删除。
 */
void
SharedFileSetInit(SharedFileSet *fileset, dsm_segment *seg)
{
	/* 初始化共享文件集特有的成员。 */
	SpinLockInit(&fileset->mutex);
	fileset->refcnt = 1;

	/* 初始化该文件集。 */
	FileSetInit(&fileset->fs);

	/* 登记我们的清理回调。 */
	if (seg)
		on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * 挂接到用 SharedFileSetInit 创建的一组目录。
 */
void
SharedFileSetAttach(SharedFileSet *fileset, dsm_segment *seg)
{
	bool		success;

	SpinLockAcquire(&fileset->mutex);
	if (fileset->refcnt == 0)
		success = false;
	else
	{
		++fileset->refcnt;
		success = true;
	}
	SpinLockRelease(&fileset->mutex);

	if (!success)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not attach to a SharedFileSet that is already destroyed")));

	/* 登记我们的清理回调。 */
	on_dsm_detach(seg, SharedFileSetOnDetach, PointerGetDatum(fileset));
}

/*
 * 删除集合里的所有文件。
 */
void
SharedFileSetDeleteAll(SharedFileSet *fileset)
{
	FileSetDeleteAll(&fileset->fs);
}

/*
 * 当本后端脱离一个持有它所创建或挂接的 SharedFileSet 的
 * DSM 段时，会调用这个回调函数。若
 * 我们是最后一个脱离的，就尝试去掉这些目录以及
 * 其中的一切。失败时没法抛出错误，因为这运行在
 * 错误清理路径上。
 */
static void
SharedFileSetOnDetach(dsm_segment *segment, Datum datum)
{
	bool		unlink_all = false;
	SharedFileSet *fileset = (SharedFileSet *) DatumGetPointer(datum);

	SpinLockAcquire(&fileset->mutex);
	Assert(fileset->refcnt > 0);
	if (--fileset->refcnt == 0)
		unlink_all = true;
	SpinLockRelease(&fileset->mutex);

	/*
	 * 若我们是最后一个脱离的，就删除所有
	 * 表空间里的目录。注意，本函数其余部分我们实际上仍然挂接着，
	 * 因此可以安全地访问它的数据。
	 */
	if (unlink_all)
		FileSetDeleteAll(&fileset->fs);
}
