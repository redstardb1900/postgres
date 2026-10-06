/*-------------------------------------------------------------------------
 *
 * clogdesc.c
 *	  access/transam/clog.c 的资源管理器描述例程
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/clogdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/clog.h"


void
clog_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == CLOG_ZEROPAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "page %d", pageno);
	}
	else if (info == CLOG_TRUNCATE)
	{
		xl_clog_truncate xlrec;

		memcpy(&xlrec, rec, sizeof(xl_clog_truncate));
		appendStringInfo(buf, "page %d; oldestXact %u",
						 xlrec.pageno, xlrec.oldestXact);
	}
}

const char *
clog_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case CLOG_ZEROPAGE:
			id = "ZEROPAGE";
			break;
		case CLOG_TRUNCATE:
			id = "TRUNCATE";
			break;
	}

	return id;
}
