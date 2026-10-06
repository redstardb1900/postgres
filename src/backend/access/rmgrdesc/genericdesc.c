/*-------------------------------------------------------------------------
 *
 * genericdesc.c
 *	  access/transam/generic_xlog.c 的资源管理器描述例程
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/rmgrdesc/genericdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

/*
 * 通用 xlog 记录的描述：写出本记录所覆盖的页
 * 区域。
 */
void
generic_desc(StringInfo buf, XLogReaderState *record)
{
	Pointer		ptr = XLogRecGetData(record),
				end = ptr + XLogRecGetDataLen(record);

	while (ptr < end)
	{
		OffsetNumber offset,
					length;

		memcpy(&offset, ptr, sizeof(offset));
		ptr += sizeof(offset);
		memcpy(&length, ptr, sizeof(length));
		ptr += sizeof(length);
		ptr += length;

		if (ptr < end)
			appendStringInfo(buf, "offset %u, length %u; ", offset, length);
		else
			appendStringInfo(buf, "offset %u, length %u", offset, length);
	}
}

/*
 * 通用 xlog 记录的标识：我们不区分通用 xlog
 * 记录内部的任何子类型。
 */
const char *
generic_identify(uint8 info)
{
	return "Generic";
}
