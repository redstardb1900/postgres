/*-------------------------------------------------------------------------
 *
 * pseudotypes.c
 *	  系统伪类型的函数。
 *
 * 伪类型并不是真正的类型，也没有任何操作，但
 * 我们确实需要提供输入与输出函数，以满足
 * pg_type 中伪类型条目内的链接。多数情况下，这些
 * 函数被调用时只是抛出错误。（XXX 这些错误消息
 * 覆盖最常见情形，但在某些上下文可能令人
 * 困惑。）
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/pseudotypes.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rangetypes.h"
#include "utils/multirangetypes.h"


/*
 * 这些宏为一个伪类型生成拒绝一切输入与输出
 * 尝试的输入与输出函数。（但对某些类型，仅
 * 输入函数须为哑。）
 */
#define PSEUDOTYPE_DUMMY_INPUT_FUNC(typname) \
Datum \
typname##_in(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot accept a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

#define PSEUDOTYPE_DUMMY_IO_FUNCS(typname) \
PSEUDOTYPE_DUMMY_INPUT_FUNC(typname); \
\
Datum \
typname##_out(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot display a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

/*
 * 二进制 send/receive 函数同理。许多伪类型我们
 * 根本不为其提供这些函数，但有些有。（按惯例，
 * 若类型有 send 函数就应有 receive 函数，即便那
 * 只是占位。）
 */
#define PSEUDOTYPE_DUMMY_RECEIVE_FUNC(typname) \
Datum \
typname##_recv(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot accept a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable

#define PSEUDOTYPE_DUMMY_BINARY_IO_FUNCS(typname) \
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(typname); \
\
Datum \
typname##_send(PG_FUNCTION_ARGS) \
{ \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("cannot display a value of type %s", #typname))); \
\
	PG_RETURN_VOID();			/* keep compiler quiet */ \
} \
\
extern int no_such_variable


/*
 * cstring
 *
 * cstring 被标记为伪类型，是因为我们不想让人在
 * 表中使用它。但它其实是一个功能完备的类型，
 * 所以为它提供一整套可用的 I/O 函数。除此之外，
 * 这允许手工调用数据类型 I/O 函数，例如
 * "SELECT foo_in('blah')" or "SELECT foo_out(some-foo-value)".
 */
Datum
cstring_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

Datum
cstring_out(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_CSTRING(pstrdup(str));
}

Datum
cstring_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	PG_RETURN_CSTRING(str);
}

Datum
cstring_send(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendtext(&buf, str, strlen(str));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * anyarray
 *
 * 我们需要允许 anyarray 的输出，例如 pg_statistic
 * 列能够被打印。但输入必须被禁止。
 *
 * XXX anyarray_recv 其实可以做成可用，因为传入的
 * 数组数据会包含元素类型 OID。但它似乎不太可能
 * 足够类型安全。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyarray);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(anyarray);

Datum
anyarray_out(PG_FUNCTION_ARGS)
{
	return array_out(fcinfo);
}

Datum
anyarray_send(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 * anycompatiblearray
 *
 * 对 anyarray 已允许输出，这里不妨照做。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblearray);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(anycompatiblearray);

Datum
anycompatiblearray_out(PG_FUNCTION_ARGS)
{
	return array_out(fcinfo);
}

Datum
anycompatiblearray_send(PG_FUNCTION_ARGS)
{
	return array_send(fcinfo);
}

/*
 * anyenum
 *
 * 不妨允许输出，因为 enum_out 实际能用。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyenum);

Datum
anyenum_out(PG_FUNCTION_ARGS)
{
	return enum_out(fcinfo);
}

/*
 * anyrange
 *
 * 不妨允许输出，因为 range_out 实际能用。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anyrange);

Datum
anyrange_out(PG_FUNCTION_ARGS)
{
	return range_out(fcinfo);
}

/*
 * anycompatiblerange
 *
 * 不妨允许输出，因为 range_out 实际能用。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblerange);

Datum
anycompatiblerange_out(PG_FUNCTION_ARGS)
{
	return range_out(fcinfo);
}

/*
 * anymultirange
 *
 * 不妨允许输出，因为 multirange_out 实际能用。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anymultirange);

Datum
anymultirange_out(PG_FUNCTION_ARGS)
{
	return multirange_out(fcinfo);
}

/*
 * anycompatiblemultirange
 *
 * 不妨允许输出，因为 multirange_out 实际能用。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(anycompatiblemultirange);

Datum
anycompatiblemultirange_out(PG_FUNCTION_ARGS)
{
	return multirange_out(fcinfo);
}

/*
 * void
 *
 * 我们支持 void_in，让 PL 函数无需 PL 处理器中的
 * 特殊处理就能返回 VOID。PL 认为自己返回的值
 * 都只会被忽略。反过来，void_out 与 void_send 是
 * 必需的，以让 "SELECT function_returning_void(...)" 工作
 */
Datum
void_in(PG_FUNCTION_ARGS)
{
	PG_RETURN_VOID();			/* 没料到吧？ */
}

Datum
void_out(PG_FUNCTION_ARGS)
{
	PG_RETURN_CSTRING(pstrdup(""));
}

Datum
void_recv(PG_FUNCTION_ARGS)
{
	/*
	 * 注意：由于我们不消耗任何字节，试图发送空串
	 * 以外的东西将导致"invalid message format"错误。
	 */
	PG_RETURN_VOID();
}

Datum
void_send(PG_FUNCTION_ARGS)
{
	StringInfoData buf;

	/* 发送空串。 */
	pq_begintypsend(&buf);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * shell
 *
 * 对于"shell"类型（尚未填充的类型），pg_type 中
 * 会登记 shell_in 与 shell_out。它们本应不可达，
 * 还是设置了它们，以防某些代码路径没有检查
 * 沿途任何位置的 pg_type.typisdefined 就做 I/O。
 */
Datum
shell_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of a shell type")));

	PG_RETURN_VOID();			/* 使编译器安静 */
}

Datum
shell_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot display a value of a shell type")));

	PG_RETURN_VOID();			/* 使编译器安静 */
}


/*
 * pg_node_tree
 *
 * pg_node_tree 并非真正的伪类型 --- 它已真实到可以
 * 作为表列存在 --- 但它目前没有自己的操作，也不
 * 允许输入，其 I/O 函数放哪里都行，放这里亦可。
 *
 * 我们必须禁止 pg_node_tree 值的输入，因为操作该
 * 类型的 SQL 函数对畸形输入并无防护。
 * 不过输出是要允许的。
 */
PSEUDOTYPE_DUMMY_INPUT_FUNC(pg_node_tree);
PSEUDOTYPE_DUMMY_RECEIVE_FUNC(pg_node_tree);

Datum
pg_node_tree_out(PG_FUNCTION_ARGS)
{
	return textout(fcinfo);
}

Datum
pg_node_tree_send(PG_FUNCTION_ARGS)
{
	return textsend(fcinfo);
}

/*
 * pg_ddl_command
 *
 * 与 pg_node_tree 一样，pg_ddl_command 并非真正的伪
 * 类型；理由与前者相同。
 *
 * 我们没有任何直接输出该类型的好办法，所以
 * 输入输出均不处理。
 */
PSEUDOTYPE_DUMMY_IO_FUNCS(pg_ddl_command);
PSEUDOTYPE_DUMMY_BINARY_IO_FUNCS(pg_ddl_command);


/*
 * 其他多种伪类型的占位 I/O 函数。
 */
PSEUDOTYPE_DUMMY_IO_FUNCS(any);
PSEUDOTYPE_DUMMY_IO_FUNCS(trigger);
PSEUDOTYPE_DUMMY_IO_FUNCS(event_trigger);
PSEUDOTYPE_DUMMY_IO_FUNCS(language_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(fdw_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(table_am_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(index_am_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(tsm_handler);
PSEUDOTYPE_DUMMY_IO_FUNCS(internal);
PSEUDOTYPE_DUMMY_IO_FUNCS(anyelement);
PSEUDOTYPE_DUMMY_IO_FUNCS(anynonarray);
PSEUDOTYPE_DUMMY_IO_FUNCS(anycompatible);
PSEUDOTYPE_DUMMY_IO_FUNCS(anycompatiblenonarray);
