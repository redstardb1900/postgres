/*-------------------------------------------------------------------------
 *
 * varchar.c
 *	  内置类型 char(n) 与 varchar(n) 的函数。
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varchar.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"

/* bpchartypmodin 与 varchartypmodin 的公共代码 */
static int32
anychar_typmodin(ArrayType *ta, const char *typename)
{
	int32		typmod;
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * 此处不细究错误消息好坏，因为文法不应允许
	 * CHAR 的修饰符数量错误。
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s must be at least 1", typename)));
	if (*tl > MaxAttrSize)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s cannot exceed %d",
						typename, MaxAttrSize)));

	/*
	 * 主要出于历史原因，typmod 是 VARHDRSZ 加上字符数；
	 * 有太多客户端代码知道这一点，最好不要改变它。
	 *
	 */
	typmod = VARHDRSZ + *tl;

	return typmod;
}

/* bpchartypmodout 与 varchartypmodout 的公共代码 */
static char *
anychar_typmodout(int32 typmod)
{
	char	   *res = (char *) palloc(64);

	if (typmod > VARHDRSZ)
		snprintf(res, 64, "(%d)", (int) (typmod - VARHDRSZ));
	else
		*res = '\0';

	return res;
}


/*
 * CHAR() 与 VARCHAR() 类型是 SQL 标准的一部分。CHAR()
 * 为空白填充的字符串，其长度在 CREATE TABLE 中指定。
 * VARCHAR 用于存储长度至多为 CREATE TABLE 时所指定长度的
 * 字符串。
 *
 * 这些类型难实现，因为我们无法从类型本身得知
 * 类型的长度。我修改（希望是全部）那些调用数据类型
 * 输入函数的 fmgr 调用，让它们同时提供长度。例如
 * 在 INSERT 语句中，我们有包含属性长度的 tupleDescriptor，
 * 因而可知 char() 或 varchar() 的确切长度。我们把它
 * 传给 bpcharin() 或 varcharin()。在无法确定长度的
 * 情形下，改为传入 -1，此时输入转换器不执行任何
 * 长度检查。
 *
 * 我们实际将其实现为 varlena，这样比较函数就无需
 * 传入长度。这些类型与 "text" 的区别是插入时可能
 * 截断并填充。
 *
 *
 *															  - ay 6/95
 */


/*****************************************************************************
 *	 bpchar - char()														 *
 *****************************************************************************/

/*
 * bpchar_input -- bpcharin/bpcharrecv 的公共主体
 *
 * s 是长度为 len 的输入文本（未必是 NUL 结尾）
 * atttypmod 是要应用的 typmod 值
 *
 * 注意 atttypmod 的计量单位是字符，
 * 这与字节数未必相同。
 *
 * 若输入字符串过长则报错，除非多出的字符都是
 * 空格，此时将其截断。（依 SQL）
 *
 * 若 escontext 指向 ErrorSaveContext 节点，则填充该节点
 * 而非抛出错误；调用者须检查 SOFT_ERROR_OCCURRED()
 * 检测错误。
 */
static BpChar *
bpchar_input(const char *s, size_t len, int32 atttypmod, Node *escontext)
{
	BpChar	   *result;
	char	   *r;
	size_t		maxlen;

	/* 若 typmod 为 -1 或无效，则用实际字符串长度 */
	if (atttypmod < (int32) VARHDRSZ)
		maxlen = len;
	else
	{
		size_t		charlen;	/* 输入中的字符个数 */

		maxlen = atttypmod - VARHDRSZ;
		charlen = pg_mbstrlen_with_len(s, len);
		if (charlen > maxlen)
		{
			/* 校验多出的字符是否为空格，并将其剪除 */
			size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);
			size_t		j;

			/*
			 * 此时，len 是输入的实际 BYTE 长度，
			 * maxlen 是此 bpchar 类型允许的最大字符数，
			 * mbmaxlen 是这些字符的 BYTE 长度。
			 */
			for (j = mbmaxlen; j < len; j++)
			{
				if (s[j] != ' ')
					ereturn(escontext, NULL,
							(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
							 errmsg("value too long for type character(%d)",
									(int) maxlen)));
			}

			/*
			 * 现在将 maxlen 设为所需的字节长度，而非
			 * 字符数！
			 */
			maxlen = len = mbmaxlen;
		}
		else
		{
			/*
			 * 现在将 maxlen 设为所需的字节长度，而非
			 * 字符数！
			 */
			maxlen = len + (maxlen - charlen);
		}
	}

	result = (BpChar *) palloc(maxlen + VARHDRSZ);
	SET_VARSIZE(result, maxlen + VARHDRSZ);
	r = VARDATA(result);
	memcpy(r, s, len);

	/* 必要时以空格填充字符串 */
	if (maxlen > len)
		memset(r + len, ' ', maxlen - len);

	return result;
}

/*
 * 将 C 字符串转为 CHARACTER 内部表示。atttypmod
 * 是类型的声明长度加上 VARHDRSZ。
 */
Datum
bpcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	BpChar	   *result;

	result = bpchar_input(s, strlen(s), atttypmod, fcinfo->context);
	PG_RETURN_BPCHAR_P(result);
}


/*
 * 将 CHARACTER 值转为 C 字符串。
 *
 * 使用 text 转换函数，这仅在 BpChar 与 text 是等价
 * 类型时才合适。
 */
Datum
bpcharout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 *		bpcharrecv			- 将外部二进制格式转为 bpchar
 */
Datum
bpcharrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	BpChar	   *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	result = bpchar_input(str, nbytes, atttypmod, NULL);
	pfree(str);
	PG_RETURN_BPCHAR_P(result);
}

/*
 *		bpcharsend		- 将 bpchar 转为二进制格式
 */
Datum
bpcharsend(PG_FUNCTION_ARGS)
{
	/* 与 textsend 完全相同，故共享代码 */
	return textsend(fcinfo);
}


/*
 * 将 CHARACTER 类型转换为指定大小。
 *
 * maxlen 是 typmod，即声明长度加 VARHDRSZ 字节。
 * isExplicit 为真表示这是到 char(N) 的显式转换。
 *
 * 截断规则：显式转换静默截断到给定长度；隐式转换
 * 则报错，除非多出的字符全为空格。这大致依 SQL：
 * 标准其实要求我们对显式转换引发 "完成条件"，但
 * Postgres 并无此概念。
 *
 */
Datum
bpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *source = PG_GETARG_BPCHAR_PP(0);
	int32		maxlen = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	BpChar	   *result;
	int32		len;
	char	   *r;
	char	   *s;
	int			i;
	int			charlen;		/* 输入字符串中的字符数 +
								 * VARHDRSZ */

	/* typmod 无效时无需处理 */
	if (maxlen < (int32) VARHDRSZ)
		PG_RETURN_BPCHAR_P(source);

	maxlen -= VARHDRSZ;

	len = VARSIZE_ANY_EXHDR(source);
	s = VARDATA_ANY(source);

	charlen = pg_mbstrlen_with_len(s, len);

	/* 传入数据已符合 typmod 时无需处理 */
	if (charlen == maxlen)
		PG_RETURN_BPCHAR_P(source);

	if (charlen > maxlen)
	{
		/* 校验多出的字符是否为空格，并将其剪除 */
		size_t		maxmblen;

		maxmblen = pg_mbcharcliplen(s, len, maxlen);

		if (!isExplicit)
		{
			for (i = maxmblen; i < len; i++)
				if (s[i] != ' ')
					ereport(ERROR,
							(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
							 errmsg("value too long for type character(%d)",
									maxlen)));
		}

		len = maxmblen;

		/*
		 * 此时 maxlen 即所需的字节长度，而非
		 * 字符数！
		 */
		maxlen = len;
	}
	else
	{
		/*
		 * 此时 maxlen 即所需的字节长度，而非
		 * 字符数！
		 */
		maxlen = len + (maxlen - charlen);
	}

	Assert(maxlen >= len);

	result = palloc(maxlen + VARHDRSZ);
	SET_VARSIZE(result, maxlen + VARHDRSZ);
	r = VARDATA(result);

	memcpy(r, s, len);

	/* 必要时以空格填充字符串 */
	if (maxlen > len)
		memset(r + len, ' ', maxlen - len);

	PG_RETURN_BPCHAR_P(result);
}


/* char_bpchar()
 * char 转为 bpchar(1)。
 */
Datum
char_bpchar(PG_FUNCTION_ARGS)
{
	char		c = PG_GETARG_CHAR(0);
	BpChar	   *result;

	result = (BpChar *) palloc(VARHDRSZ + 1);

	SET_VARSIZE(result, VARHDRSZ + 1);
	*(VARDATA(result)) = c;

	PG_RETURN_BPCHAR_P(result);
}


/* bpchar_name()
 * 将 bpchar() 类型转为 NameData 类型。
 */
Datum
bpchar_name(PG_FUNCTION_ARGS)
{
	BpChar	   *s = PG_GETARG_BPCHAR_PP(0);
	char	   *s_data;
	Name		result;
	int			len;

	len = VARSIZE_ANY_EXHDR(s);
	s_data = VARDATA_ANY(s);

	/* 截断超长输入 */
	if (len >= NAMEDATALEN)
		len = pg_mbcliplen(s_data, len, NAMEDATALEN - 1);

	/* 移除尾部空白 */
	while (len > 0)
	{
		if (s_data[len - 1] != ' ')
			break;
		len--;
	}

	/* 此处用 palloc0 确保结果零填充 */
	result = (Name) palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), s_data, len);

	PG_RETURN_NAME(result);
}

/* name_bpchar()
 * 将 NameData 类型转为 bpchar 类型。
 *
 * 使用 text 转换函数，这仅在 BpChar 与 text 是等价
 * 类型时才合适。
 */
Datum
name_bpchar(PG_FUNCTION_ARGS)
{
	Name		s = PG_GETARG_NAME(0);
	BpChar	   *result;

	result = (BpChar *) cstring_to_text(NameStr(*s));
	PG_RETURN_BPCHAR_P(result);
}

Datum
bpchartypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anychar_typmodin(ta, "char"));
}

Datum
bpchartypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anychar_typmodout(typmod));
}


/*****************************************************************************
 *	 varchar - varchar(n)
 *
 * 注意：varchar 在大多数操作上借用 text 类型，因此除
 * I/O 与 typmod 检查外没有 C 编码函数。
 *****************************************************************************/

/*
 * varchar_input -- varcharin/varcharrecv 的公共主体
 *
 * s 是长度为 len 的输入文本（未必是 NUL 结尾）
 * atttypmod 是要应用的 typmod 值
 *
 * 注意 atttypmod 的计量单位是字符，
 * 这与字节数未必相同。
 *
 * 若输入字符串过长则报错，除非多出的字符都是
 * 空格，此时将其截断。（依 SQL）
 *
 * 若 escontext 指向 ErrorSaveContext 节点，则填充该节点
 * 而非抛出错误；调用者须检查 SOFT_ERROR_OCCURRED()
 * 检测错误。
 */
static VarChar *
varchar_input(const char *s, size_t len, int32 atttypmod, Node *escontext)
{
	VarChar    *result;
	size_t		maxlen;

	maxlen = atttypmod - VARHDRSZ;

	if (atttypmod >= (int32) VARHDRSZ && len > maxlen)
	{
		/* 校验多出的字符是否为空格，并将其剪除 */
		size_t		mbmaxlen = pg_mbcharcliplen(s, len, maxlen);
		size_t		j;

		for (j = mbmaxlen; j < len; j++)
		{
			if (s[j] != ' ')
				ereturn(escontext, NULL,
						(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						 errmsg("value too long for type character varying(%d)",
								(int) maxlen)));
		}

		len = mbmaxlen;
	}

	/*
	 * 可以用 cstring_to_text_with_len，因为 VarChar 与 text
	 * 二进制兼容。
	 */
	result = (VarChar *) cstring_to_text_with_len(s, len);
	return result;
}

/*
 * 将 C 字符串转为 VARCHAR 内部表示。atttypmod
 * 是类型的声明长度加上 VARHDRSZ。
 */
Datum
varcharin(PG_FUNCTION_ARGS)
{
	char	   *s = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar    *result;

	result = varchar_input(s, strlen(s), atttypmod, fcinfo->context);
	PG_RETURN_VARCHAR_P(result);
}


/*
 * 将 VARCHAR 值转为 C 字符串。
 *
 * 使用 text 转 C 字符串的转换函数，这仅在 VarChar
 * 与 text 是等价类型时才合适。
 */
Datum
varcharout(PG_FUNCTION_ARGS)
{
	Datum		txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 *		varcharrecv			- 将外部二进制格式转为 varchar
 */
Datum
varcharrecv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarChar    *result;
	char	   *str;
	int			nbytes;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	result = varchar_input(str, nbytes, atttypmod, NULL);
	pfree(str);
	PG_RETURN_VARCHAR_P(result);
}

/*
 *		varcharsend		- 将 varchar 转为二进制格式
 */
Datum
varcharsend(PG_FUNCTION_ARGS)
{
	/* 与 textsend 完全相同，故共享代码 */
	return textsend(fcinfo);
}


/*
 * varchar_support()
 *
 * varchar() 长度强制转换函数的规划器支持函数。
 *
 * 目前我们能做的唯一有意义的事是把新最大长度
 * 设为不小于旧最大长度的调用展平。isExplicit 参数可以
 * 忽略，因为它只影响截断情形。
 */
Datum
varchar_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		FuncExpr   *expr = req->fcall;
		Node	   *typmod;

		Assert(list_length(expr->args) >= 2);

		typmod = (Node *) lsecond(expr->args);

		if (IsA(typmod, Const) && !((Const *) typmod)->constisnull)
		{
			Node	   *source = (Node *) linitial(expr->args);
			int32		old_typmod = exprTypmod(source);
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			int32		old_max = old_typmod - VARHDRSZ;
			int32		new_max = new_typmod - VARHDRSZ;

			if (new_typmod < 0 || (old_typmod >= 0 && old_max <= new_max))
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * 将 VARCHAR 类型转换为指定大小。
 *
 * maxlen 是 typmod，即声明长度加 VARHDRSZ 字节。
 * isExplicit 为真表示这是到 varchar(N) 的显式转换。
 *
 * 截断规则：显式转换静默截断到给定长度；隐式转换
 * 则报错，除非多出的字符全为空格。这大致依 SQL：
 * 标准其实要求我们对显式转换引发 "完成条件"，但
 * Postgres 并无此概念。
 *
 */
Datum
varchar(PG_FUNCTION_ARGS)
{
	VarChar    *source = PG_GETARG_VARCHAR_PP(0);
	int32		typmod = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	int32		len,
				maxlen;
	size_t		maxmblen;
	int			i;
	char	   *s_data;

	len = VARSIZE_ANY_EXHDR(source);
	s_data = VARDATA_ANY(source);
	maxlen = typmod - VARHDRSZ;

	/* typmod 无效或传入数据已符合时无需处理 */
	if (maxlen < 0 || len <= maxlen)
		PG_RETURN_VARCHAR_P(source);

	/* 字符串过长才会到这里… */

	/* 截断多字节字符串，保留多字节边界 */
	maxmblen = pg_mbcharcliplen(s_data, len, maxlen);

	if (!isExplicit)
	{
		for (i = maxmblen; i < len; i++)
			if (s_data[i] != ' ')
				ereport(ERROR,
						(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						 errmsg("value too long for type character varying(%d)",
								maxlen)));
	}

	PG_RETURN_VARCHAR_P((VarChar *) cstring_to_text_with_len(s_data,
															 maxmblen));
}

Datum
varchartypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anychar_typmodin(ta, "varchar"));
}

Datum
varchartypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anychar_typmodout(typmod));
}


/*****************************************************************************
 * 导出函数
 *****************************************************************************/

/* BpChar 的 "真实" 长度（不计尾部空白） */
static inline int
bcTruelen(BpChar *arg)
{
	return bpchartruelen(VARDATA_ANY(arg), VARSIZE_ANY_EXHDR(arg));
}

int
bpchartruelen(char *s, int len)
{
	int			i;

	/*
	 * 我们依赖如下假设：' ' 在所有受支持的多字节
	 * 服务器编码上是单一单元。
	 */
	for (i = len - 1; i >= 0; i--)
	{
		if (s[i] != ' ')
			break;
	}
	return i + 1;
}

Datum
bpcharlen(PG_FUNCTION_ARGS)
{
	BpChar	   *arg = PG_GETARG_BPCHAR_PP(0);
	int			len;

	/* 取字节数，忽略尾部空格 */
	len = bcTruelen(arg);

	/* 多字节编码时，转换为字符数 */
	if (pg_database_encoding_max_length() != 1)
		len = pg_mbstrlen_with_len(VARDATA_ANY(arg), len);

	PG_RETURN_INT32(len);
}

Datum
bpcharoctetlen(PG_FUNCTION_ARGS)
{
	Datum		arg = PG_GETARG_DATUM(0);

	/* 完全无需 detoast 输入 */
	PG_RETURN_INT32(toast_raw_datum_size(arg) - VARHDRSZ);
}


/*****************************************************************************
 *	bpchar 用的比较函数
 *
 * 注意：btree 索引要求这些例程不泄漏内存；因此，
 * 对 toast 化 datum 的工作副本要小心释放。大多数地方
 * 不必如此小心。
 *****************************************************************************/

static void
check_collation_set(Oid collid)
{
	if (!OidIsValid(collid))
	{
		/*
		 * 通常意味着解析器无法解决隐式排序规则冲突，
		 * 因此按该方式报告。
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

Datum
bpchareq(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	bool		result;
	Oid			collid = PG_GET_COLLATION();
	bool		locale_is_c = false;
	pg_locale_t mylocale = 0;

	check_collation_set(collid);

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	if (lc_collate_is_c(collid))
		locale_is_c = true;
	else
		mylocale = pg_newlocale_from_collation(collid);

	if (locale_is_c || pg_locale_deterministic(mylocale))
	{
		/*
		 * 由于我们只关心相等或不相等，这里可以省去
		 * strcoll() 的开销，只做按位比较。
		 */
		if (len1 != len2)
			result = false;
		else
			result = (memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), len1) == 0);
	}
	else
	{
		result = (varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
							 collid) == 0);
	}

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharne(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	bool		result;
	Oid			collid = PG_GET_COLLATION();
	bool		locale_is_c = false;
	pg_locale_t mylocale = 0;

	check_collation_set(collid);

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	if (lc_collate_is_c(collid))
		locale_is_c = true;
	else
		mylocale = pg_newlocale_from_collation(collid);

	if (locale_is_c || pg_locale_deterministic(mylocale))
	{
		/*
		 * 由于我们只关心相等或不相等，这里可以省去
		 * strcoll() 的开销，只做按位比较。
		 */
		if (len1 != len2)
			result = true;
		else
			result = (memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), len1) != 0);
	}
	else
	{
		result = (varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
							 collid) != 0);
	}

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bpcharlt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp < 0);
}

Datum
bpcharle(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp <= 0);
}

Datum
bpchargt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp > 0);
}

Datum
bpcharge(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(cmp >= 0);
}

Datum
bpcharcmp(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(cmp);
}

Datum
bpchar_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	Oid			collid = ssup->ssup_collation;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 用通用字符串 SortSupport */
	varstr_sortsupport(ssup, BPCHAROID, collid);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}

Datum
bpchar_larger(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_RETURN_BPCHAR_P((cmp >= 0) ? arg1 : arg2);
}

Datum
bpchar_smaller(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			len1,
				len2;
	int			cmp;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	cmp = varstr_cmp(VARDATA_ANY(arg1), len1, VARDATA_ANY(arg2), len2,
					 PG_GET_COLLATION());

	PG_RETURN_BPCHAR_P((cmp <= 0) ? arg1 : arg2);
}


/*
 * bpchar 需要专用的哈希函数，因为我们想在比较中
 * 忽略尾部空白。
 */
Datum
hashbpchar(PG_FUNCTION_ARGS)
{
	BpChar	   *key = PG_GETARG_BPCHAR_PP(0);
	Oid			collid = PG_GET_COLLATION();
	char	   *keydata;
	int			keylen;
	pg_locale_t mylocale = 0;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	keydata = VARDATA_ANY(key);
	keylen = bcTruelen(key);

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (pg_locale_deterministic(mylocale))
	{
		result = hash_any((unsigned char *) keydata, keylen);
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;

		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用可能返回比第一次更小的值 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
		 * 原则上没理由把 NUL 终止符包含进哈希，但
		 * 之前却这样做了，且行为必须保留。
		 *
		 */
		result = hash_any((uint8_t *) buf, bsize + 1);

		pfree(buf);
	}

	/* 避免 toast 输入的内存泄漏 */
	PG_FREE_IF_COPY(key, 0);

	return result;
}

Datum
hashbpcharextended(PG_FUNCTION_ARGS)
{
	BpChar	   *key = PG_GETARG_BPCHAR_PP(0);
	Oid			collid = PG_GET_COLLATION();
	char	   *keydata;
	int			keylen;
	pg_locale_t mylocale = 0;
	Datum		result;

	if (!collid)
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string hashing"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));

	keydata = VARDATA_ANY(key);
	keylen = bcTruelen(key);

	if (!lc_collate_is_c(collid))
		mylocale = pg_newlocale_from_collation(collid);

	if (pg_locale_deterministic(mylocale))
	{
		result = hash_any_extended((unsigned char *) keydata, keylen,
								   PG_GETARG_INT64(1));
	}
	else
	{
		Size		bsize,
					rsize;
		char	   *buf;

		bsize = pg_strnxfrm(NULL, 0, keydata, keylen, mylocale);
		buf = palloc(bsize + 1);

		rsize = pg_strnxfrm(buf, bsize + 1, keydata, keylen, mylocale);

		/* 第二次调用可能返回比第一次更小的值 */
		if (rsize > bsize)
			elog(ERROR, "pg_strnxfrm() returned unexpected result");

		/*
		 * 原则上没理由把 NUL 终止符包含进哈希，但
		 * 之前却这样做了，且行为必须保留。
		 *
		 */
		result = hash_any_extended((uint8_t *) buf, bsize + 1,
								   PG_GETARG_INT64(1));

		pfree(buf);
	}

	PG_FREE_IF_COPY(key, 0);

	return result;
}

/*
 * 以下算子支持对 bpchar datum 的逐字符比较，
 * 以便构建适用于 LIKE 子句的索引。注意，常规的
 * bpchareq/bpcharne 比较算子，以及采用 "C" 排序规则
 * 的常规支持函数 1 和 2 被假定与这些算子兼容！
 *
 */

static int
internal_bpchar_pattern_compare(BpChar *arg1, BpChar *arg2)
{
	int			result;
	int			len1,
				len2;

	len1 = bcTruelen(arg1);
	len2 = bcTruelen(arg2);

	result = memcmp(VARDATA_ANY(arg1), VARDATA_ANY(arg2), Min(len1, len2));
	if (result != 0)
		return result;
	else if (len1 < len2)
		return -1;
	else if (len1 > len2)
		return 1;
	else
		return 0;
}


Datum
bpchar_pattern_lt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result < 0);
}


Datum
bpchar_pattern_le(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result <= 0);
}


Datum
bpchar_pattern_ge(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result >= 0);
}


Datum
bpchar_pattern_gt(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result > 0);
}


Datum
btbpchar_pattern_cmp(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1 = PG_GETARG_BPCHAR_PP(0);
	BpChar	   *arg2 = PG_GETARG_BPCHAR_PP(1);
	int			result;

	result = internal_bpchar_pattern_compare(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}


Datum
btbpchar_pattern_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

	/* 用通用字符串 SortSupport（"C" 排序规则） */
	varstr_sortsupport(ssup, BPCHAROID, C_COLLATION_OID);

	MemoryContextSwitchTo(oldcontext);

	PG_RETURN_VOID();
}
