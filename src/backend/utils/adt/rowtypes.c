/*-------------------------------------------------------------------------
 *
 * rowtypes.c
 *	  通用复合类型的 I/O 与比较函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/rowtypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"


/*
 * 缓存 record I/O 所需元数据的结构
 */
typedef struct ColumnIOData
{
	Oid			column_type;
	Oid			typiofunc;
	Oid			typioparam;
	bool		typisvarlena;
	FmgrInfo	proc;
} ColumnIOData;

typedef struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	int			ncolumns;
	ColumnIOData columns[FLEXIBLE_ARRAY_MEMBER];
} RecordIOData;

/*
 * 缓存 record 比较所需元数据的结构
 */
typedef struct ColumnCompareData
{
	TypeCacheEntry *typentry;	/* 其实已含所需的一切 */
} ColumnCompareData;

typedef struct RecordCompareData
{
	int			ncolumns;		/* columns[] 的分配长度 */
	Oid			record1_type;
	int32		record1_typmod;
	Oid			record2_type;
	int32		record2_typmod;
	ColumnCompareData columns[FLEXIBLE_ARRAY_MEMBER];
} RecordCompareData;


/*
 * record_in		- 任意复合类型的输入例程。
 */
Datum
record_in(PG_FUNCTION_ARGS)
{
	char	   *string = PG_GETARG_CSTRING(0);
	Oid			tupType = PG_GETARG_OID(1);
	int32		tupTypmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	HeapTupleHeader result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	RecordIOData *my_extra;
	bool		needComma = false;
	int			ncolumns;
	int			i;
	char	   *ptr;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/*
	 * 若没有得到足够的信息来标识目标 record 类型，则给出
	 * 友好的错误消息。（lookup_rowtype_tupdesc 反正也会
	 * 失败，但报出的消息对用户不友好。）在普通 SQL 用法
	 * 中 typmod 会是 -1，因为复合类型和 RECORD 在 SQL 层面
	 * 上没有类型修饰符，故对 RECORD 必然失败。不过
	 * 有些调用者可提供有效 typmod，能为 RECORD 做有用的事
	 */
	if (tupType == RECORDOID && tupTypmod < 0)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("input of anonymous composite types is not implemented")));

	/*
	 * 这来自复合类型的 pg_type.oid 并把系统 oid
	 * 存入用户表，即 DatumTupleFields。该 oid 必须
	 * 由二进制升级保留。
	 */
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/*
	 * 我们安排每轮调用只查一次所需的 I/O 信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/*
	 * 扫描字符串。用 "buf" 累积每列的去引号数据，随后
	 * 将其送入相应的输入转换器。
	 */
	ptr = string;
	/* 允许前导空白 */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;
	if (*ptr++ != '(')
	{
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Missing left parenthesis.")));
		goto fail;
	}

	initStringInfo(&buf);

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		char	   *column_data;

		/* 忽略类型中被丢弃的列，但填入 null */
		if (att->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		if (needComma)
		{
			/* 跳过分隔前一个字段与当前字段的逗号 */
			if (*ptr == ',')
				ptr++;
			else
				/* *ptr 须是 ')' */
			{
				errsave(escontext,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("malformed record literal: \"%s\"", string),
						 errdetail("Too few columns.")));
				goto fail;
			}
		}

		/* 检查 null：输入完全为空即 null */
		if (*ptr == ',' || *ptr == ')')
		{
			column_data = NULL;
			nulls[i] = true;
		}
		else
		{
			/* 提取该列的字符串 */
			bool		inquote = false;

			resetStringInfo(&buf);
			while (inquote || !(*ptr == ',' || *ptr == ')'))
			{
				char		ch = *ptr++;

				if (ch == '\0')
				{
					errsave(escontext,
							(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
							 errmsg("malformed record literal: \"%s\"",
									string),
							 errdetail("Unexpected end of input.")));
					goto fail;
				}
				if (ch == '\\')
				{
					if (*ptr == '\0')
					{
						errsave(escontext,
								(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
								 errmsg("malformed record literal: \"%s\"",
										string),
								 errdetail("Unexpected end of input.")));
						goto fail;
					}
					appendStringInfoChar(&buf, *ptr++);
				}
				else if (ch == '"')
				{
					if (!inquote)
						inquote = true;
					else if (*ptr == '"')
					{
						/* 引号序列内的双写引号 */
						appendStringInfoChar(&buf, *ptr++);
					}
					else
						inquote = false;
				}
				else
					appendStringInfoChar(&buf, ch);
			}

			column_data = buf.data;
			nulls[i] = false;
		}

		/*
		 * 转换该列的值
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		if (!InputFunctionCallSafe(&column_info->proc,
								   column_data,
								   column_info->typioparam,
								   att->atttypmod,
								   escontext,
								   &values[i]))
			goto fail;

		/*
		 * 准备下一列
		 */
		needComma = true;
	}

	if (*ptr++ != ')')
	{
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Too many columns.")));
		goto fail;
	}
	/* 允许结尾空白 */
	while (*ptr && isspace((unsigned char) *ptr))
		ptr++;
	if (*ptr)
	{
		errsave(escontext,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("malformed record literal: \"%s\"", string),
				 errdetail("Junk after right parenthesis.")));
		goto fail;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * 我们不能返回 tuple->t_data，因为 heap_form_tuple 把它作为
	 * 更大内存块的一部分来分配，而调用者可能期望能够
	 * pfree 它。必须把信息复制到新的 palloc 块。
	 */
	result = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy(result, tuple->t_data, tuple->t_len);

	heap_freetuple(tuple);
	pfree(buf.data);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_HEAPTUPLEHEADER(result);

	/* 做完 lookup_rowtype_tupdesc 后由此退出 */
fail:
	ReleaseTupleDesc(tupdesc);
	PG_RETURN_NULL();
}

/*
 * record_out		- 任意复合类型的输出例程。
 */
Datum
record_out(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	bool		needComma = false;
	int			ncolumns;
	int			i;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组本身提取类型信息 */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* 构建临时的 HeapTuple 控制结构 */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	/*
	 * 我们安排每轮调用只查一次所需的 I/O 信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* 把元组拆解成各个字段 */
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	/* 然后构建结果字符串 */
	initStringInfo(&buf);

	appendStringInfoChar(&buf, '(');

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Datum		attr;
		char	   *value;
		char	   *tmp;
		bool		nq;

		/* 忽略类型中被丢弃的列 */
		if (att->attisdropped)
			continue;

		if (needComma)
			appendStringInfoChar(&buf, ',');
		needComma = true;

		if (nulls[i])
		{
			/* 不输出... */
			continue;
		}

		/*
		 * 把列值转换为文本
		 */
		if (column_info->column_type != column_type)
		{
			getTypeOutputInfo(column_type,
							  &column_info->typiofunc,
							  &column_info->typisvarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		attr = values[i];
		value = OutputFunctionCall(&column_info->proc, attr);

		/* 检测该值是否需要双引号 */
		nq = (value[0] == '\0');	/* 空串要强制加引号 */
		for (tmp = value; *tmp; tmp++)
		{
			char		ch = *tmp;

			if (ch == '"' || ch == '\\' ||
				ch == '(' || ch == ')' || ch == ',' ||
				isspace((unsigned char) ch))
			{
				nq = true;
				break;
			}
		}

		/* 再输出字符串 */
		if (nq)
			appendStringInfoCharMacro(&buf, '"');
		for (tmp = value; *tmp; tmp++)
		{
			char		ch = *tmp;

			if (ch == '"' || ch == '\\')
				appendStringInfoCharMacro(&buf, ch);
			appendStringInfoCharMacro(&buf, ch);
		}
		if (nq)
			appendStringInfoCharMacro(&buf, '"');
	}

	appendStringInfoChar(&buf, ')');

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_CSTRING(buf.data);
}

/*
 * record_recv		- 任意复合类型二进制输入例程。
 */
Datum
record_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	Oid			tupType = PG_GETARG_OID(1);
	int32		tupTypmod = PG_GETARG_INT32(2);
	HeapTupleHeader result;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			usercols;
	int			validcols;
	int			i;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/*
	 * 若没有得到足够的信息来标识目标 record 类型，则给出
	 * 友好的错误消息。（lookup_rowtype_tupdesc 反正也会
	 * 失败，但报出的消息对用户不友好。）在普通 SQL 用法
	 * 中 typmod 会是 -1，因为复合类型和 RECORD 在 SQL 层面
	 * 上没有类型修饰符，故对 RECORD 必然失败。不过
	 * 有些调用者可提供有效 typmod，能为 RECORD 做有用的事
	 */
	if (tupType == RECORDOID && tupTypmod < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("input of anonymous composite types is not implemented")));

	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/*
	 * 我们安排每轮调用只查一次所需的 I/O 信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* 取用户认为它具有的列数 */
	usercols = pq_getmsgint(buf, 4);

	/* 需要扫描以统计未删除的列数 */
	validcols = 0;
	for (i = 0; i < ncolumns; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			validcols++;
	}
	if (usercols != validcols)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("wrong number of columns: %d, expected %d",
						usercols, validcols)));

	/* 处理每一列 */
	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Oid			coltypoid;
		int			itemlen;
		StringInfoData item_buf;
		StringInfo	bufptr;
		char		csave;

		/* 忽略类型中被丢弃的列，但填入 null */
		if (att->attisdropped)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
			continue;
		}

		/* 检查数据中记录的列类型 */
		coltypoid = pq_getmsgint(buf, sizeof(Oid));

		/*
		 * 从安全角度看，输入的列类型是否与期望相符并
		 * 无关紧要：列类型的接收函数必须健壮到能
		 * 应对无效数据。但从易用性角度看，
		 * 对类型不匹配提出抱怨胜过抛出 "improper binary
		 * format" 错误。但有个问题：仅内建类型
		 * 才拥有稳定到足以相信不匹配是真问题的 OID。
		 * 所以仅当两个 OID 都在
		 * 内建范围内时才提出抱怨。
		 * 否则，继续使用我们"本应"得到的列类型。
		 */
		if (coltypoid != column_type &&
			coltypoid < FirstGenbkiObjectId &&
			column_type < FirstGenbkiObjectId)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("binary data has type %u (%s) instead of expected %u (%s) in record column %d",
							coltypoid,
							format_type_extended(coltypoid, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							column_type,
							format_type_extended(column_type, -1,
												 FORMAT_TYPE_ALLOW_INVALID),
							i + 1)));

		/* 取并检查条目长度 */
		itemlen = pq_getmsgint(buf, 4);
		if (itemlen < -1 || itemlen > (buf->len - buf->cursor))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("insufficient data left in message")));

		if (itemlen == -1)
		{
			/* -1 长度即 NULL */
			bufptr = NULL;
			nulls[i] = true;
			csave = 0;			/* 让编译器安静 */
		}
		else
		{
			/*
			 * 与其复制数据，我们只设置一个虚假的
			 * StringInfo，让它指向输入缓冲区的正确部分。
			 * 我们假定可以在输入缓冲区上随意涂写，以维持
			 * StringInfo 带有结尾 null 的约定。
			 */
			item_buf.data = &buf->data[buf->cursor];
			item_buf.maxlen = itemlen + 1;
			item_buf.len = itemlen;
			item_buf.cursor = 0;

			buf->cursor += itemlen;

			csave = buf->data[buf->cursor];
			buf->data[buf->cursor] = '\0';

			bufptr = &item_buf;
			nulls[i] = false;
		}

		/* 现在调用该列的 receiveproc */
		if (column_info->column_type != column_type)
		{
			getTypeBinaryInputInfo(column_type,
								   &column_info->typiofunc,
								   &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		values[i] = ReceiveFunctionCall(&column_info->proc,
										bufptr,
										column_info->typioparam,
										att->atttypmod);

		if (bufptr)
		{
			/* 没吃掉整个缓冲区就有麻烦 */
			if (item_buf.cursor != itemlen)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
						 errmsg("improper binary format in record column %d",
								i + 1)));

			buf->data[buf->cursor] = csave;
		}
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * 我们不能返回 tuple->t_data，因为 heap_form_tuple 把它作为
	 * 更大内存块的一部分来分配，而调用者可能期望能够
	 * pfree 它。必须把信息复制到新的 palloc 块。
	 */
	result = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy(result, tuple->t_data, tuple->t_len);

	heap_freetuple(tuple);
	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_HEAPTUPLEHEADER(result);
}

/*
 * record_send		- 任意复合类型二进制输出例程。
 */
Datum
record_send(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec = PG_GETARG_HEAPTUPLEHEADER(0);
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			validcols;
	int			i;
	Datum	   *values;
	bool	   *nulls;
	StringInfoData buf;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组本身提取类型信息 */
	tupType = HeapTupleHeaderGetTypeId(rec);
	tupTypmod = HeapTupleHeaderGetTypMod(rec);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* 构建临时的 HeapTuple 控制结构 */
	tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = rec;

	/*
	 * 我们安排每轮调用只查一次所需的 I/O 信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	/* 把元组拆解成各个字段 */
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	/* 然后构建结果字符串 */
	pq_begintypsend(&buf);

	/* 需要扫描以统计未删除的列数 */
	validcols = 0;
	for (i = 0; i < ncolumns; i++)
	{
		if (!TupleDescAttr(tupdesc, i)->attisdropped)
			validcols++;
	}
	pq_sendint32(&buf, validcols);

	for (i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ColumnIOData *column_info = &my_extra->columns[i];
		Oid			column_type = att->atttypid;
		Datum		attr;
		bytea	   *outputbytes;

		/* 忽略类型中被丢弃的列 */
		if (att->attisdropped)
			continue;

		pq_sendint32(&buf, column_type);

		if (nulls[i])
		{
			/* 输出 -1 数据长度以表示 NULL */
			pq_sendint32(&buf, -1);
			continue;
		}

		/*
		 * 把列值转换为二进制
		 */
		if (column_info->column_type != column_type)
		{
			getTypeBinaryOutputInfo(column_type,
									&column_info->typiofunc,
									&column_info->typisvarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		attr = values[i];
		outputbytes = SendFunctionCall(&column_info->proc, attr);
		pq_sendint32(&buf, VARSIZE(outputbytes) - VARHDRSZ);
		pq_sendbytes(&buf, VARDATA(outputbytes),
					 VARSIZE(outputbytes) - VARHDRSZ);
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * record_cmp()
 * record 的内部比较函数。
 *
 * 返回 -1/0/1
 *
 * 不要假定两个输入是完全相同的 record 类型；
 * 例如可能是在拿匿名的 ROW() 构造与命名的复合类型
 * 相比较。只要两者的非丢弃列数量相同、类型相同，
 * 我们就会进行比较。
 */
static int
record_cmp(FunctionCallInfo fcinfo)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	int			result = 0;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组提取类型信息 */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* 构建临时 HeapTuple 控制结构 */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * 我们安排每系列调用只查找一次所需的比较信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* 把元组们拆解成字段 */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * 扫描对应的列，允许丢弃列位于两行的不同位置。
	 * i1 和 i2 是物理列下标，j 是逻辑
	 * 列下标。
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;
		TypeCacheEntry *typentry;
		Oid			collation;

		/*
		 * 跳过丢弃列
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* 不匹配情况循环后再处理 */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * 有匹配的两列，其类型必须相同
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * 若排序规则不同，我们在此不抱怨，但
		 * 比较函数可能会。
		 */
		collation = att1->attcollation;
		if (collation != att2->attcollation)
			collation = InvalidOid;

		/*
		 * 若尚未完成则查找比较函数
		 */
		typentry = my_extra->columns[j].typentry;
		if (typentry == NULL ||
			typentry->type_id != att1->atttypid)
		{
			typentry = lookup_type_cache(att1->atttypid,
										 TYPECACHE_CMP_PROC_FINFO);
			if (!OidIsValid(typentry->cmp_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a comparison function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[j].typentry = typentry;
		}

		/*
		 * 两个 NULL 相等；NULL > 非 NULL。
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			LOCAL_FCINFO(locfcinfo, 2);
			int32		cmpresult;

			if (nulls1[i1])
			{
				/* arg1 大于 arg2 */
				result = 1;
				break;
			}
			if (nulls2[i2])
			{
				/* arg1 小于 arg2 */
				result = -1;
				break;
			}

			/* 比较这一对元素 */
			InitFunctionCallInfoData(*locfcinfo, &typentry->cmp_proc_finfo, 2,
									 collation, NULL, NULL);
			locfcinfo->args[0].value = values1[i1];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = values2[i2];
			locfcinfo->args[1].isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(locfcinfo));

			/* 我们不期望比较支持函数返回 null */
			Assert(!locfcinfo->isnull);

			if (cmpresult < 0)
			{
				/* arg1 小于 arg2 */
				result = -1;
				break;
			}
			else if (cmpresult > 0)
			{
				/* arg1 大于 arg2 */
				result = 1;
				break;
			}
		}

		/* 相等，继续处理下一列 */
		i1++, i2++, j++;
	}

	/*
	 * 若未提前跳出循环，则检查列数是否不匹配。
	 * （如果我们已经发现了不相等的列值，就不报告这种
	 * 不匹配；特性还是缺陷？）
	 */
	if (result == 0)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	return result;
}

/*
 * record_eq:
 *		  对两个 record 比较相等性
 * 结果:
 *		  record 相等返回 true，否则 false。
 *
 * 注意：我们在这里不使用 record_cmp，因为相等性在
 * 没有全序（故无 btree 支持）的类型中可能有意义。
 */
Datum
record_eq(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	bool		result = true;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组提取类型信息 */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* 构建临时 HeapTuple 控制结构 */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * 我们安排每系列调用只查找一次所需的比较信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* 把元组们拆解成字段 */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * 扫描对应的列，允许丢弃列位于两行的不同位置。
	 * i1 和 i2 是物理列下标，j 是逻辑
	 * 列下标。
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		LOCAL_FCINFO(locfcinfo, 2);
		Form_pg_attribute att1;
		Form_pg_attribute att2;
		TypeCacheEntry *typentry;
		Oid			collation;
		bool		oprresult;

		/*
		 * 跳过丢弃列
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* 不匹配情况循环后再处理 */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * 有匹配的两列，其类型必须相同
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * 若排序规则不同，我们在此不抱怨，但
		 * 相等函数可能会。
		 */
		collation = att1->attcollation;
		if (collation != att2->attcollation)
			collation = InvalidOid;

		/*
		 * 若尚未完成则查找相等函数
		 */
		typentry = my_extra->columns[j].typentry;
		if (typentry == NULL ||
			typentry->type_id != att1->atttypid)
		{
			typentry = lookup_type_cache(att1->atttypid,
										 TYPECACHE_EQ_OPR_FINFO);
			if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an equality operator for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[j].typentry = typentry;
		}

		/*
		 * 两个 NULL 相等；NULL > 非 NULL。
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			if (nulls1[i1] || nulls2[i2])
			{
				result = false;
				break;
			}

			/* 比较这一对元素 */
			InitFunctionCallInfoData(*locfcinfo, &typentry->eq_opr_finfo, 2,
									 collation, NULL, NULL);
			locfcinfo->args[0].value = values1[i1];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = values2[i2];
			locfcinfo->args[1].isnull = false;
			oprresult = DatumGetBool(FunctionCallInvoke(locfcinfo));
			if (locfcinfo->isnull || !oprresult)
			{
				result = false;
				break;
			}
		}

		/* 相等，继续处理下一列 */
		i1++, i2++, j++;
	}

	/*
	 * 若未提前跳出循环，则检查列数是否不匹配。
	 * （如果我们已经发现了不相等的列值，就不报告这种
	 * 不匹配；特性还是缺陷？）
	 */
	if (result)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	PG_RETURN_BOOL(result);
}

Datum
record_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(record_eq(fcinfo)));
}

Datum
record_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) < 0);
}

Datum
record_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) > 0);
}

Datum
record_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) <= 0);
}

Datum
record_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_cmp(fcinfo) >= 0);
}

Datum
btrecordcmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(record_cmp(fcinfo));
}


/*
 * record_image_cmp:
 * record 的按字节比较的内部函数。
 *
 * 返回 -1/0/1
 *
 * 注意："相等"的正常概念在此不适用；被视为相等的
 * 值的两种不同表示不被视为同一。举例来说，对于
 * citext 类型 "A" 与 "a" 相等，但它们并
 * 不同一。
 */
static int
record_image_cmp(FunctionCallInfo fcinfo)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	int			result = 0;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	/* 从元组提取类型信息 */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* 构建临时 HeapTuple 控制结构 */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * 我们安排每系列调用只查找一次所需的比较信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* 把元组们拆解成字段 */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * 扫描对应的列，允许丢弃列位于两行的不同位置。
	 * i1 和 i2 是物理列下标，j 是逻辑
	 * 列下标。
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;

		/*
		 * 跳过丢弃列
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* 不匹配情况循环后再处理 */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * 有匹配的两列，其类型必须相同
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * 同类型应当有相同的长度，或两者均为
		 * 变长。
		 */
		Assert(att1->attlen == att2->attlen);

		/*
		 * 两个 NULL 相等；NULL > 非 NULL。
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			int			cmpresult = 0;

			if (nulls1[i1])
			{
				/* arg1 大于 arg2 */
				result = 1;
				break;
			}
			if (nulls2[i2])
			{
				/* arg1 小于 arg2 */
				result = -1;
				break;
			}

			/* 比较这一对元素 */
			if (att1->attbyval)
			{
				if (values1[i1] != values2[i2])
					cmpresult = (values1[i1] < values2[i2]) ? -1 : 1;
			}
			else if (att1->attlen > 0)
			{
				cmpresult = memcmp(DatumGetPointer(values1[i1]),
								   DatumGetPointer(values2[i2]),
								   att1->attlen);
			}
			else if (att1->attlen == -1)
			{
				Size		len1,
							len2;
				struct varlena *arg1val;
				struct varlena *arg2val;

				len1 = toast_raw_datum_size(values1[i1]);
				len2 = toast_raw_datum_size(values2[i2]);
				arg1val = PG_DETOAST_DATUM_PACKED(values1[i1]);
				arg2val = PG_DETOAST_DATUM_PACKED(values2[i2]);

				cmpresult = memcmp(VARDATA_ANY(arg1val),
								   VARDATA_ANY(arg2val),
								   Min(len1, len2) - VARHDRSZ);
				if ((cmpresult == 0) && (len1 != len2))
					cmpresult = (len1 < len2) ? -1 : 1;

				if ((Pointer) arg1val != (Pointer) values1[i1])
					pfree(arg1val);
				if ((Pointer) arg2val != (Pointer) values2[i2])
					pfree(arg2val);
			}
			else
				elog(ERROR, "unexpected attlen: %d", att1->attlen);

			if (cmpresult < 0)
			{
				/* arg1 小于 arg2 */
				result = -1;
				break;
			}
			else if (cmpresult > 0)
			{
				/* arg1 大于 arg2 */
				result = 1;
				break;
			}
		}

		/* 相等，继续处理下一列 */
		i1++, i2++, j++;
	}

	/*
	 * 若未提前跳出循环，则检查列数是否不匹配。
	 * （如果我们已经发现了不相等的列值，就不报告这种
	 * 不匹配；特性还是缺陷？）
	 */
	if (result == 0)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	return result;
}

/*
 * record_image_eq:
 *		  基于字节映像比较两个 record 的内容是否同一
 * 结果:
 *		  record 同一返回 true，否则 false。
 *
 * 注意：这里不用 record_image_cmp，因为这样
 * 可避免对不等长度做解压缩。
 */
Datum
record_image_eq(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record1 = PG_GETARG_HEAPTUPLEHEADER(0);
	HeapTupleHeader record2 = PG_GETARG_HEAPTUPLEHEADER(1);
	bool		result = true;
	Oid			tupType1;
	Oid			tupType2;
	int32		tupTypmod1;
	int32		tupTypmod2;
	TupleDesc	tupdesc1;
	TupleDesc	tupdesc2;
	HeapTupleData tuple1;
	HeapTupleData tuple2;
	int			ncolumns1;
	int			ncolumns2;
	RecordCompareData *my_extra;
	int			ncols;
	Datum	   *values1;
	Datum	   *values2;
	bool	   *nulls1;
	bool	   *nulls2;
	int			i1;
	int			i2;
	int			j;

	/* 从元组提取类型信息 */
	tupType1 = HeapTupleHeaderGetTypeId(record1);
	tupTypmod1 = HeapTupleHeaderGetTypMod(record1);
	tupdesc1 = lookup_rowtype_tupdesc(tupType1, tupTypmod1);
	ncolumns1 = tupdesc1->natts;
	tupType2 = HeapTupleHeaderGetTypeId(record2);
	tupTypmod2 = HeapTupleHeaderGetTypMod(record2);
	tupdesc2 = lookup_rowtype_tupdesc(tupType2, tupTypmod2);
	ncolumns2 = tupdesc2->natts;

	/* 构建临时 HeapTuple 控制结构 */
	tuple1.t_len = HeapTupleHeaderGetDatumLength(record1);
	ItemPointerSetInvalid(&(tuple1.t_self));
	tuple1.t_tableOid = InvalidOid;
	tuple1.t_data = record1;
	tuple2.t_len = HeapTupleHeaderGetDatumLength(record2);
	ItemPointerSetInvalid(&(tuple2.t_self));
	tuple2.t_tableOid = InvalidOid;
	tuple2.t_data = record2;

	/*
	 * 我们安排每系列调用只查找一次所需的比较信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	ncols = Max(ncolumns1, ncolumns2);
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncols)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncols * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncols;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
		my_extra->record2_type = InvalidOid;
		my_extra->record2_typmod = 0;
	}

	if (my_extra->record1_type != tupType1 ||
		my_extra->record1_typmod != tupTypmod1 ||
		my_extra->record2_type != tupType2 ||
		my_extra->record2_typmod != tupTypmod2)
	{
		MemSet(my_extra->columns, 0, ncols * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType1;
		my_extra->record1_typmod = tupTypmod1;
		my_extra->record2_type = tupType2;
		my_extra->record2_typmod = tupTypmod2;
	}

	/* 把元组们拆解成字段 */
	values1 = (Datum *) palloc(ncolumns1 * sizeof(Datum));
	nulls1 = (bool *) palloc(ncolumns1 * sizeof(bool));
	heap_deform_tuple(&tuple1, tupdesc1, values1, nulls1);
	values2 = (Datum *) palloc(ncolumns2 * sizeof(Datum));
	nulls2 = (bool *) palloc(ncolumns2 * sizeof(bool));
	heap_deform_tuple(&tuple2, tupdesc2, values2, nulls2);

	/*
	 * 扫描对应的列，允许丢弃列位于两行的不同位置。
	 * i1 和 i2 是物理列下标，j 是逻辑
	 * 列下标。
	 */
	i1 = i2 = j = 0;
	while (i1 < ncolumns1 || i2 < ncolumns2)
	{
		Form_pg_attribute att1;
		Form_pg_attribute att2;

		/*
		 * 跳过丢弃列
		 */
		if (i1 < ncolumns1 && TupleDescAttr(tupdesc1, i1)->attisdropped)
		{
			i1++;
			continue;
		}
		if (i2 < ncolumns2 && TupleDescAttr(tupdesc2, i2)->attisdropped)
		{
			i2++;
			continue;
		}
		if (i1 >= ncolumns1 || i2 >= ncolumns2)
			break;				/* 不匹配情况循环后再处理 */

		att1 = TupleDescAttr(tupdesc1, i1);
		att2 = TupleDescAttr(tupdesc2, i2);

		/*
		 * 有匹配的两列，其类型必须相同
		 */
		if (att1->atttypid != att2->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare dissimilar column types %s and %s at record column %d",
							format_type_be(att1->atttypid),
							format_type_be(att2->atttypid),
							j + 1)));

		/*
		 * 两个 NULL 相等；NULL > 非 NULL。
		 */
		if (!nulls1[i1] || !nulls2[i2])
		{
			if (nulls1[i1] || nulls2[i2])
			{
				result = false;
				break;
			}

			/* 比较这一对元素 */
			result = datum_image_eq(values1[i1], values2[i2], att1->attbyval, att2->attlen);
			if (!result)
				break;
		}

		/* 相等，继续处理下一列 */
		i1++, i2++, j++;
	}

	/*
	 * 若未提前跳出循环，则检查列数是否不匹配。
	 * （如果我们已经发现了不相等的列值，就不报告这种
	 * 不匹配；特性还是缺陷？）
	 */
	if (result)
	{
		if (i1 != ncolumns1 || i2 != ncolumns2)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot compare record types with different numbers of columns")));
	}

	pfree(values1);
	pfree(nulls1);
	pfree(values2);
	pfree(nulls2);
	ReleaseTupleDesc(tupdesc1);
	ReleaseTupleDesc(tupdesc2);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record1, 0);
	PG_FREE_IF_COPY(record2, 1);

	PG_RETURN_BOOL(result);
}

Datum
record_image_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!DatumGetBool(record_image_eq(fcinfo)));
}

Datum
record_image_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) < 0);
}

Datum
record_image_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) > 0);
}

Datum
record_image_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) <= 0);
}

Datum
record_image_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(record_image_cmp(fcinfo) >= 0);
}

Datum
btrecordimagecmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(record_image_cmp(fcinfo));
}


/*
 * 行类型哈希函数
 */

Datum
hash_record(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record = PG_GETARG_HEAPTUPLEHEADER(0);
	uint32		result = 0;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	int			ncolumns;
	RecordCompareData *my_extra;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组提取类型信息 */
	tupType = HeapTupleHeaderGetTypeId(record);
	tupTypmod = HeapTupleHeaderGetTypMod(record);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* 构建 HeapTuple 临时控制结构 */
	tuple.t_len = HeapTupleHeaderGetDatumLength(record);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = record;

	/*
	 * 我们安排每轮调用只查一次所需的哈希信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncolumns * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncolumns;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
	}

	if (my_extra->record1_type != tupType ||
		my_extra->record1_typmod != tupTypmod)
	{
		MemSet(my_extra->columns, 0, ncolumns * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType;
		my_extra->record1_typmod = tupTypmod;
	}

	/* 把元组拆解成各个字段 */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	for (int i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;
		uint32		element_hash;

		att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		/*
		 * 若尚未完成则查找哈希函数
		 */
		typentry = my_extra->columns[i].typentry;
		if (typentry == NULL ||
			typentry->type_id != att->atttypid)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_HASH_PROC_FINFO);
			if (!OidIsValid(typentry->hash_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify a hash function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[i].typentry = typentry;
		}

		/* 计算元素的哈希 */
		if (nulls[i])
		{
			element_hash = 0;
		}
		else
		{
			LOCAL_FCINFO(locfcinfo, 1);

			InitFunctionCallInfoData(*locfcinfo, &typentry->hash_proc_finfo, 1,
									 att->attcollation, NULL, NULL);
			locfcinfo->args[0].value = values[i];
			locfcinfo->args[0].isnull = false;
			element_hash = DatumGetUInt32(FunctionCallInvoke(locfcinfo));

			/* 我们不期望哈希支持函数返回 null */
			Assert(!locfcinfo->isnull);
		}

		/* 见hash_array() */
		result = (result << 5) - result + element_hash;
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record, 0);

	PG_RETURN_UINT32(result);
}

Datum
hash_record_extended(PG_FUNCTION_ARGS)
{
	HeapTupleHeader record = PG_GETARG_HEAPTUPLEHEADER(0);
	uint64		seed = PG_GETARG_INT64(1);
	uint64		result = 0;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	int			ncolumns;
	RecordCompareData *my_extra;
	Datum	   *values;
	bool	   *nulls;

	check_stack_depth();		/* 为 record 类型列而递归 */

	/* 从元组提取类型信息 */
	tupType = HeapTupleHeaderGetTypeId(record);
	tupTypmod = HeapTupleHeaderGetTypMod(record);
	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
	ncolumns = tupdesc->natts;

	/* 构建 HeapTuple 临时控制结构 */
	tuple.t_len = HeapTupleHeaderGetDatumLength(record);
	ItemPointerSetInvalid(&(tuple.t_self));
	tuple.t_tableOid = InvalidOid;
	tuple.t_data = record;

	/*
	 * 我们安排每轮调用只查一次所需的哈希信息，
	 * 前提是 record 类型不会在我们脚下改变。
	 */
	my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns < ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordCompareData, columns) +
							   ncolumns * sizeof(ColumnCompareData));
		my_extra = (RecordCompareData *) fcinfo->flinfo->fn_extra;
		my_extra->ncolumns = ncolumns;
		my_extra->record1_type = InvalidOid;
		my_extra->record1_typmod = 0;
	}

	if (my_extra->record1_type != tupType ||
		my_extra->record1_typmod != tupTypmod)
	{
		MemSet(my_extra->columns, 0, ncolumns * sizeof(ColumnCompareData));
		my_extra->record1_type = tupType;
		my_extra->record1_typmod = tupTypmod;
	}

	/* 把元组拆解成各个字段 */
	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));
	heap_deform_tuple(&tuple, tupdesc, values, nulls);

	for (int i = 0; i < ncolumns; i++)
	{
		Form_pg_attribute att;
		TypeCacheEntry *typentry;
		uint64		element_hash;

		att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;

		/*
		 * 若尚未完成则查找哈希函数
		 */
		typentry = my_extra->columns[i].typentry;
		if (typentry == NULL ||
			typentry->type_id != att->atttypid)
		{
			typentry = lookup_type_cache(att->atttypid,
										 TYPECACHE_HASH_EXTENDED_PROC_FINFO);
			if (!OidIsValid(typentry->hash_extended_proc_finfo.fn_oid))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_FUNCTION),
						 errmsg("could not identify an extended hash function for type %s",
								format_type_be(typentry->type_id))));
			my_extra->columns[i].typentry = typentry;
		}

		/* 计算元素的哈希 */
		if (nulls[i])
		{
			element_hash = 0;
		}
		else
		{
			LOCAL_FCINFO(locfcinfo, 2);

			InitFunctionCallInfoData(*locfcinfo, &typentry->hash_extended_proc_finfo, 2,
									 att->attcollation, NULL, NULL);
			locfcinfo->args[0].value = values[i];
			locfcinfo->args[0].isnull = false;
			locfcinfo->args[1].value = Int64GetDatum(seed);
			locfcinfo->args[0].isnull = false;
			element_hash = DatumGetUInt64(FunctionCallInvoke(locfcinfo));

			/* 我们不期望哈希支持函数返回 null */
			Assert(!locfcinfo->isnull);
		}

		/* 见 hash_array_extended() */
		result = (result << 5) - result + element_hash;
	}

	pfree(values);
	pfree(nulls);
	ReleaseTupleDesc(tupdesc);

	/* 收到 toast 输入时避免内存泄漏。 */
	PG_FREE_IF_COPY(record, 0);

	PG_RETURN_UINT64(result);
}
