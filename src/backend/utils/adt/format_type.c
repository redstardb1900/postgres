/*-------------------------------------------------------------------------
 *
 * format_type.c
 *	  美观地显示类型名。
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/format_type.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/syscache.h"

static char *printTypmod(const char *typname, int32 typmod, Oid typmodout);


/*
 * SQL 函数：format_type(type_oid, typemod)
 *
 * type_oid 来自 pg_type.oid，typemod 来自
 * pg_attribute.atttypmod。本函数会取得类型名并
 * 把类型名与修饰符格式化成规范 SQL 格式，前提
 * 是该类型为标准类型；否则只会取回
 * pg_type.typname，怪异字符或关键字相同时加双引号。
 *
 * 如果 typemod 为 NULL，说明我们是在无 typemod
 * 可用的上下文中格式化类型名，例如函数参数或
 * 结果类型。这会与在某些情况下指定 typemod
 * = -1 产生略有不同的结果。给定 typemod =
 * -1 时，我们觉得必须产生一个解析器会解释
 * 为 typemod -1 的输出，让 pg_dump 能产生重建
 * 原始状态的 CREATE TABLE 命令。但若给定 NULL
 *  typemod，我们就假定解析器对 typemod 的解释无
 * 关紧要，所以我们愿意输出同一类型"美观"
 * 的表示。例如 type = bpchar 且 typemod = NULL 得
 * 到 "character"；typemod = -1 得到 "bpchar"。前者会解
 * 释成 character(1)，而非 -1。
 *
 * XXX 在 typemod = NULL 中编码一种含义很丑陋；
 * 更干净的做法是分别提供一参和两参两个函数。
 * 不过现在不值得改。
 */
Datum
format_type(PG_FUNCTION_ARGS)
{
	Oid			type_oid;
	int32		typemod;
	char	   *result;
	bits16		flags = FORMAT_TYPE_ALLOW_INVALID;

	/* 由于本函数不是严格的，我们必须测试空参数 */
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	type_oid = PG_GETARG_OID(0);

	if (PG_ARGISNULL(1))
		typemod = -1;
	else
	{
		typemod = PG_GETARG_INT32(1);
		flags |= FORMAT_TYPE_TYPEMOD_GIVEN;
	}

	result = format_type_extended(type_oid, typemod, flags);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * format_type_extended
 *		生成可能带限定的类型名。
 *
 * 默认行为是：仅当类型不在搜索路径中时才加限
 * 定，忽略给定的 typmod，并且在给定的 type_oid 不存在
 * 时报错。
 *
 * 'flags' 中的下列位会修改行为：
 * - FORMAT_TYPE_TYPEMOD_GIVEN
 *			在输出中包含 typmod（typmod 仍可能为 -1）
 * - FORMAT_TYPE_ALLOW_INVALID
 *			若类型 OID 无效或未知，返回 ??? 之类而非
 *			失败。
 * - FORMAT_TYPE_INVALID_AS_NULL
 *			若类型 OID 无效或未知，返回 NULL 而非 ??? 之
 *			类
 * - FORMAT_TYPE_FORCE_QUALIFY
 *			总是给类型名加上模式限定，无视 search_path
 *
 * 注意 TYPEMOD_GIVEN 与 "typemod == -1" 不可互换；
 * 参见上文 format_type() 的注释。
 *
 * 返回 palloc 的串或 NULL。
 */
char *
format_type_extended(Oid type_oid, int32 typemod, bits16 flags)
{
	HeapTuple	tuple;
	Form_pg_type typeform;
	Oid			array_base_type;
	bool		is_array;
	char	   *buf;
	bool		with_typemod;

	if (type_oid == InvalidOid)
	{
		if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
			return NULL;
		else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
			return pstrdup("-");
	}

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
	{
		if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
			return NULL;
		else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
			return pstrdup("???");
		else
			elog(ERROR, "cache lookup failed for type %u", type_oid);
	}
	typeform = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * 检查它是否是"真正的"数组类型。像 "name" 这类伪
	 * 数组类型不应被解构。还要检查 toast 属性，并且不
	 * 要解构"plain storage"的数组类型 —— 这是因为我们不
	 * 想把oidvector显示为oid[]。
	 */
	array_base_type = typeform->typelem;

	if (IsTrueArrayType(typeform) &&
		typeform->typstorage != TYPSTORAGE_PLAIN)
	{
		/* 把注意力切换到数组元素类型 */
		ReleaseSysCache(tuple);
		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(array_base_type));
		if (!HeapTupleIsValid(tuple))
		{
			if ((flags & FORMAT_TYPE_INVALID_AS_NULL) != 0)
				return NULL;
			else if ((flags & FORMAT_TYPE_ALLOW_INVALID) != 0)
				return pstrdup("???[]");
			else
				elog(ERROR, "cache lookup failed for type %u", type_oid);
		}
		typeform = (Form_pg_type) GETSTRUCT(tuple);
		type_oid = array_base_type;
		is_array = true;
	}
	else
		is_array = false;

	with_typemod = (flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0 && (typemod >= 0);

	/*
	 * 看看是否要对某些内置类型的输出做特殊处理。
	 * 注意这些特殊情况都应与 gram.y 中的特殊产
	 * 式对应，以确保类型名被当作系统类型，而不是
	 * 同名用户类型。
	 *
	 * 如果我们不在这里提供特殊输出，类型名的处理
	 * 方式将与用户类型名相同 —— 特别是，当它匹配任
	 * 何词法关键字时都要加双引号。这种行为对某
	 * 些情况至关重要，如类型 "bit" 与 "char"。
	 */
	buf = NULL;					/* 无特殊情形的标志 */

	switch (type_oid)
	{
		case BITOID:
			if (with_typemod)
				buf = printTypmod("bit", typemod, typeform->typmodout);
			else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0)
			{
				/*
				 * 带 typmod -1 的 bit 不等同于 BIT，后者意为
				 * BIT(1)。要把它报告为带引号类型名，以免解
				 * 析器指派伪造的 typmod。
				 */
			}
			else
				buf = pstrdup("bit");
			break;

		case BOOLOID:
			buf = pstrdup("boolean");
			break;

		case BPCHAROID:
			if (with_typemod)
				buf = printTypmod("character", typemod, typeform->typmodout);
			else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0)
			{
				/*
				 * 带 typmod -1 的 bpchar 不等同于 CHARACTER，后者
				 * 按 SQL 标准意为 CHARACTER(1)。把它报告为
				 * bpchar，以免解析器误设 typmod。
				 */
			}
			else
				buf = pstrdup("character");
			break;

		case FLOAT4OID:
			buf = pstrdup("real");
			break;

		case FLOAT8OID:
			buf = pstrdup("double precision");
			break;

		case INT2OID:
			buf = pstrdup("smallint");
			break;

		case INT4OID:
			buf = pstrdup("integer");
			break;

		case INT8OID:
			buf = pstrdup("bigint");
			break;

		case NUMERICOID:
			if (with_typemod)
				buf = printTypmod("numeric", typemod, typeform->typmodout);
			else
				buf = pstrdup("numeric");
			break;

		case INTERVALOID:
			if (with_typemod)
				buf = printTypmod("interval", typemod, typeform->typmodout);
			else
				buf = pstrdup("interval");
			break;

		case TIMEOID:
			if (with_typemod)
				buf = printTypmod("time", typemod, typeform->typmodout);
			else
				buf = pstrdup("time without time zone");
			break;

		case TIMETZOID:
			if (with_typemod)
				buf = printTypmod("time", typemod, typeform->typmodout);
			else
				buf = pstrdup("time with time zone");
			break;

		case TIMESTAMPOID:
			if (with_typemod)
				buf = printTypmod("timestamp", typemod, typeform->typmodout);
			else
				buf = pstrdup("timestamp without time zone");
			break;

		case TIMESTAMPTZOID:
			if (with_typemod)
				buf = printTypmod("timestamp", typemod, typeform->typmodout);
			else
				buf = pstrdup("timestamp with time zone");
			break;

		case VARBITOID:
			if (with_typemod)
				buf = printTypmod("bit varying", typemod, typeform->typmodout);
			else
				buf = pstrdup("bit varying");
			break;

		case VARCHAROID:
			if (with_typemod)
				buf = printTypmod("character varying", typemod, typeform->typmodout);
			else
				buf = pstrdup("character varying");
			break;
	}

	if (buf == NULL)
	{
		/*
		 * 默认处理：报告名字在目录中的原样。
		 * 这里若名字在搜索路径中不可见或调用者要求，
		 * 就必须加以限定；并且若它不是标准标识符或匹配
		 * 任何关键字，就必须加上双引号。
		 */
		char	   *nspname;
		char	   *typname;

		if ((flags & FORMAT_TYPE_FORCE_QUALIFY) == 0 &&
			TypeIsVisible(type_oid))
			nspname = NULL;
		else
			nspname = get_namespace_name_or_temp(typeform->typnamespace);

		typname = NameStr(typeform->typname);

		buf = quote_qualified_identifier(nspname, typname);

		if (with_typemod)
			buf = printTypmod(buf, typemod, typeform->typmodout);
	}

	if (is_array)
		buf = psprintf("%s[]", buf);

	ReleaseSysCache(tuple);

	return buf;
}

/*
 * 这个版本供后端内部在错误消息等处使用。
 * 一个差别是它对无效类型会失败。
 *
 * 结果总是 palloc 的串。
 */
char *
format_type_be(Oid type_oid)
{
	return format_type_extended(type_oid, -1, 0);
}

/*
 * 这个版本返回总是带限定的名字（除非它属于
 * TIMESTAMP WITH TIME ZONE 这类 SQL 关键字类型名）。
 */
char *
format_type_be_qualified(Oid type_oid)
{
	return format_type_extended(type_oid, -1, FORMAT_TYPE_FORCE_QUALIFY);
}

/*
 * 这个版本允许指定非默认的 typemod。
 */
char *
format_type_with_typemod(Oid type_oid, int32 typemod)
{
	return format_type_extended(type_oid, typemod, FORMAT_TYPE_TYPEMOD_GIVEN);
}

/*
 * 给基本类型名加上 typmod 修饰
 */
static char *
printTypmod(const char *typname, int32 typmod, Oid typmodout)
{
	char	   *res;

	/* typmod 为 -1 时不应调用 */
	Assert(typmod >= 0);

	if (typmodout == InvalidOid)
	{
		/* 默认行为：只打印带括号的整数 typmod */
		res = psprintf("%s(%d)", typname, (int) typmod);
	}
	else
	{
		/* 使用类型特定的 typmodout 例程 */
		char	   *tmstr;

		tmstr = DatumGetCString(OidFunctionCall1(typmodout,
												 Int32GetDatum(typmod)));
		res = psprintf("%s%s", typname, tmstr);
	}

	return res;
}


/*
 * type_maximum_size --- 确定变宽列的最大宽度
 *
 * 如果最大宽度不确定，则返回 -1。特别是，本例程
 * 对任何它不认识的类型都返回 -1。我们假定调用者
 * 已经确定该类型是变宽类型，因此无需在这里查找
 * 该类型的 pg_type 元组。
 *
 * 这看起来似乎与 format_type() 无关，但实际上两个
 * 例程共享对不同类型 typmod 编码方式的知识，所以
 * 把它们放在一起很方便。（XXX 既然这类知识大多
 * 已被移出 format_type 而进入 typmodout 函数，是否值
 * 得把这部分代码也分解出来倒是值得玩味……）
 */
int32
type_maximum_size(Oid type_oid, int32 typemod)
{
	if (typemod < 0)
		return -1;

	switch (type_oid)
	{
		case BPCHAROID:
		case VARCHAROID:
			/* typemod 含 varlena 头部 */

			/* typemod 以字符计而非字节 */
			return (typemod - VARHDRSZ) *
				pg_encoding_max_length(GetDatabaseEncoding())
				+ VARHDRSZ;

		case NUMERICOID:
			return numeric_maximum_size(typemod);

		case VARBITOID:
		case BITOID:
			/* typemod 是（最大）位数 */
			return (typemod + (BITS_PER_BYTE - 1)) / BITS_PER_BYTE
				+ 2 * sizeof(int32);
	}

	/* 未知类型或 'text' 这类宽度无限的类型 */
	return -1;
}


/*
 * oidvectortypes			- 把类型 OID 向量转换成 "typname" 列表
 */
Datum
oidvectortypes(PG_FUNCTION_ARGS)
{
	oidvector  *oidArray = (oidvector *) PG_GETARG_POINTER(0);
	char	   *result;
	int			numargs = oidArray->dim1;
	int			num;
	size_t		total;
	size_t		left;

	total = 20 * numargs + 1;
	result = palloc(total);
	result[0] = '\0';
	left = total - 1;

	for (num = 0; num < numargs; num++)
	{
		char	   *typename = format_type_extended(oidArray->values[num], -1,
													FORMAT_TYPE_ALLOW_INVALID);
		size_t		slen = strlen(typename);

		if (left < (slen + 2))
		{
			total += slen + 2;
			result = repalloc(result, total);
			left += slen + 2;
		}

		if (num > 0)
		{
			strcat(result, ", ");
			left -= 2;
		}
		strcat(result, typename);
		left -= slen;
	}

	PG_RETURN_TEXT_P(cstring_to_text(result));
}
