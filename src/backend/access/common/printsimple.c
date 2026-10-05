/*-------------------------------------------------------------------------
 *
 * printsimple.c
 *	  打印只含有限几种内建类型的元组、且不访问
 *	  系统目录的例程。面向那些没有系统目录访问能力的后端，
 *	  因为它们并未绑定到某个特定数据库，例如某些
 *	  walsender 进程。它不处理独立后端，也不处理
 *	  3.0 以外的协议版本，因为当前的应用
 *	  并不需要这种处理。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/printsimple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printsimple.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"

/*
 * 启动时发送一条 RowDescription 消息。
 */
void
printsimple_startup(DestReceiver *self, int operation, TupleDesc tupdesc)
{
	StringInfoData buf;
	int			i;

	pq_beginmessage(&buf, 'T'); /* RowDescription 消息 */
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		pq_sendstring(&buf, NameStr(attr->attname));
		pq_sendint32(&buf, 0);	/* 表 OID */
		pq_sendint16(&buf, 0);	/* 属性号 */
		pq_sendint32(&buf, (int) attr->atttypid);
		pq_sendint16(&buf, attr->attlen);
		pq_sendint32(&buf, attr->atttypmod);
		pq_sendint16(&buf, 0);	/* 格式代码 */
	}

	pq_endmessage(&buf);
}

/*
 * 对每个元组发送一条 DataRow 消息。
 */
bool
printsimple(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	StringInfoData buf;
	int			i;

	/* 确保元组已被完全拆开 */
	slot_getallattrs(slot);

	/* 准备并发送消息 */
	pq_beginmessage(&buf, 'D');
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		Datum		value;

		if (slot->tts_isnull[i])
		{
			pq_sendint32(&buf, -1);
			continue;
		}

		value = slot->tts_values[i];

		/*
		 * 这里不能调用常规的类型输出函数，因为我们
		 * 可能没有系统目录访问能力。因此必须把
		 * 所需类型的知识写死在代码里。
		 */
		switch (attr->atttypid)
		{
			case TEXTOID:
				{
					text	   *t = DatumGetTextPP(value);

					pq_sendcountedtext(&buf,
									   VARDATA_ANY(t),
									   VARSIZE_ANY_EXHDR(t),
									   false);
				}
				break;

			case INT4OID:
				{
					int32		num = DatumGetInt32(value);
					char		str[12];	/* 符号、10 位数字和 '\0' */
					int			len;

					len = pg_ltoa(num, str);
					pq_sendcountedtext(&buf, str, len, false);
				}
				break;

			case INT8OID:
				{
					int64		num = DatumGetInt64(value);
					char		str[MAXINT8LEN + 1];
					int			len;

					len = pg_lltoa(num, str);
					pq_sendcountedtext(&buf, str, len, false);
				}
				break;

			case OIDOID:
				{
					Oid			num = ObjectIdGetDatum(value);
					char		str[10];	/* 10 位数字 */
					int			len;

					len = pg_ultoa_n(num, str);
					pq_sendcountedtext(&buf, str, len, false);
				}
				break;

			default:
				elog(ERROR, "unsupported type OID: %u", attr->atttypid);
		}
	}

	pq_endmessage(&buf);

	return true;
}
