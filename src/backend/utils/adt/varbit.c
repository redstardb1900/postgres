/*-------------------------------------------------------------------------
 *
 * varbit.c
 *	  SQL 类型 BIT() 与 BIT VARYING() 的支持函数。
 *
 * 该数据结构包含以下元素：
 *     header  -- 整个数据结构的长度（含头部）
 *              （以字节计，同所有变长类型）
 *     data section -- 位数据结构专用的数据区
 *     bitlength -- 位串的位长度
 *     bitdata   -- 位串，最高有效字节在前
 *
 * bitdata 向量的长度应当总是恰好等于给定
 * 位长度所需的字节数。若位长度不是 8 的倍数，
 * 则最后一个字节中多余的低位填充位必须
 * 为零。
 *
 * attypmod 定义为位串的位长度；对变长位串则
 * 是最大长度。
 *
 * Code originally contributed by Adriaan Joubert.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/varbit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "common/int.h"
#include "libpq/pqformat.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/varbit.h"

#define HEXDIG(z)	 ((z)<10 ? ((z)+'0') : ((z)-10+'A'))

/* 屏蔽位串最后一个字节中本应为零的位 */
#define VARBIT_PAD(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*(VARBITS(vb) + VARBITBYTES(vb) - 1) &= BITMASK << pad_; \
	} while (0)

/*
 * 很多函数逐字节工作，因此手头都有一个指向
 * 末字节后位置的指针，可省一拍。
 */
#define VARBIT_PAD_LAST(vb, ptr) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		if (pad_ > 0) \
			*((ptr) - 1) &= BITMASK << pad_; \
	} while (0)

/* 断言位串填充正确 */
#ifdef USE_ASSERT_CHECKING
#define VARBIT_CORRECTLY_PADDED(vb) \
	do { \
		int32	pad_ = VARBITPAD(vb); \
		Assert(pad_ >= 0 && pad_ < BITS_PER_BYTE); \
		Assert(pad_ == 0 || \
			   (*(VARBITS(vb) + VARBITBYTES(vb) - 1) & ~(BITMASK << pad_)) == 0); \
	} while (0)
#else
#define VARBIT_CORRECTLY_PADDED(vb) ((void) 0)
#endif

static VarBit *bit_catenate(VarBit *arg1, VarBit *arg2);
static VarBit *bitsubstring(VarBit *arg, int32 s, int32 l,
							bool length_not_specified);
static VarBit *bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl);


/*
 * bittypmodin 与 varbittypmodin 的公共代码
 */
static int32
anybit_typmodin(ArrayType *ta, const char *typename)
{
	int32		typmod;
	int32	   *tl;
	int			n;

	tl = ArrayGetIntegerTypmods(ta, &n);

	/*
	 * 这里对错误信息不必太讲究，因为语法不该允许
	 * BIT 的修饰符数量出错
	 */
	if (n != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid type modifier")));

	if (*tl < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s must be at least 1",
						typename)));
	if (*tl > (MaxAttrSize * BITS_PER_BYTE))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("length for type %s cannot exceed %d",
						typename, MaxAttrSize * BITS_PER_BYTE)));

	typmod = *tl;

	return typmod;
}

/*
 * bittypmodout 与 varbittypmodout 的公共代码
 */
static char *
anybit_typmodout(int32 typmod)
{
	char	   *res = (char *) palloc(64);

	if (typmod >= 0)
		snprintf(res, 64, "(%d)", typmod);
	else
		*res = '\0';

	return res;
}


/*
 * bit_in -
 *	  把字符串转换为位串的内部表示。
 *		  长度由所需位数加上 VARHDRSZ 字节决定，或由
 *		  attypmod 决定。
 */
Datum
bit_in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	VarBit	   *result;			/* 结果位串 */
	char	   *sp;				/* 指向字符串的指针 */
	bits8	   *r;				/* 指向结果 */
	int			len,			/* 整个数据结构的长度 */
				bitlen,			/* 位串中的位数 */
				slen;			/* 输入串的长度 */
	bool		bit_not_hex;	/* false=十六进制串  true=位串 */
	int			bc;
	bits8		x = 0;

	/* 检查首个字符是 b 还是 x */
	if (input_string[0] == 'b' || input_string[0] == 'B')
	{
		bit_not_hex = true;
		sp = input_string + 1;
	}
	else if (input_string[0] == 'x' || input_string[0] == 'X')
	{
		bit_not_hex = false;
		sp = input_string + 1;
	}
	else
	{
		/*
		 * 否则按二进制处理。这使 cast('1001' as bit) 之类
		 * 写法透明可用。
		 */
		bit_not_hex = true;
		sp = input_string;
	}

	/*
	 * 从输入字符串推定位长度。MaxAllocSize 保证常规
	 * 输入足够小，但须检查十六进制输入。
	 */
	slen = strlen(sp);
	if (bit_not_hex)
		bitlen = slen;
	else
	{
		if (slen > VARBITMAXLEN / 4)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bit string length exceeds the maximum allowed (%d)",
							VARBITMAXLEN)));
		bitlen = slen * 4;
	}

	/*
	 * 有时未提供 atttypmod。若提供了它，就须确保
	 * 位串能装得下。
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen != atttypmod)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						bitlen, atttypmod)));

	len = VARBITTOTALLEN(atttypmod);
	/* 置 0 使 *r 总被初始化且字符串为零填充 */
	result = (VarBit *) palloc0(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = atttypmod;

	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* 解析字符串的位表示 */
		/* 已知装得下，bitlen 与 atttypmod 比较过 */
		x = HIGHBIT;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid binary digit",
								pg_mblen(sp), sp)));

			x >>= 1;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	else
	{
		/* 解析字符串的十六进制表示 */
		for (bc = 0; *sp; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid hexadecimal digit",
								pg_mblen(sp), sp)));

			if (bc)
			{
				*r++ |= x;
				bc = 0;
			}
			else
			{
				*r = x << 4;
				bc = 1;
			}
		}
	}

	PG_RETURN_VARBIT_P(result);
}


Datum
bit_out(PG_FUNCTION_ARGS)
{
#if 1
	/* 同 varbit 输出 */
	return varbit_out(fcinfo);
#else

	/*
	 * 这是打印十六进制字符串的方式，以备有人想
	 * 编写格式化函数。
	 */
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	char	   *result,
			   *r;
	bits8	   *sp;
	int			i,
				len,
				bitlen;

	/* 断言：帮助捕获任何未正确填充的位函数 */
	VARBIT_CORRECTLY_PADDED(s);

	bitlen = VARBITLEN(s);
	len = (bitlen + 3) / 4;
	result = (char *) palloc(len + 2);
	sp = VARBITS(s);
	r = result;
	*r++ = 'X';
	/* 我们取巧：已知存储的是零填充的整字节 */
	for (i = 0; i < len; i += 2, sp++)
	{
		*r++ = HEXDIG((*sp) >> 4);
		*r++ = HEXDIG((*sp) & 0xF);
	}

	/*
	 * 若我们打印的十六进制数字已不属于该位串，
	 * 则回退一步
	 */
	if (i > len)
		r--;
	*r = '\0';

	PG_RETURN_CSTRING(result);
#endif
}

/*
 *		bit_recv - 把外部二进制格式转换为 bit
 */
Datum
bit_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0 || bitlen > VARBITMAXLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid length in external bit string")));

	/*
	 * 有时未提供 atttypmod。若提供了它，就须确保
	 * 位串能装得下。
	 */
	if (atttypmod > 0 && bitlen != atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						bitlen, atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* 确保最后一字节零填充正确 */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

/*
 *		bit_send - 将 bit 转为二进制格式
 */
Datum
bit_send(PG_FUNCTION_ARGS)
{
	/* 与 varbit_send 完全相同，故共享代码 */
	return varbit_send(fcinfo);
}

/*
 * bit()
 * 把 bit() 类型转换为特定内部长度。
 * len 是列定义中指定的位长度。
 *
 * 隐式转换时，若源数据长度不对则报错。
 * 显式转换时，静默截断或零填充到指定长度。
 */
Datum
bit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	VarBit	   *result;
	int			rlen;

	/* typmod 无效或给定数据已与其匹配时无须处理 */
	if (len <= 0 || len > VARBITMAXLEN || len == VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	if (!isExplicit)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("bit string length %d does not match type bit(%d)",
						VARBITLEN(arg), len)));

	rlen = VARBITTOTALLEN(len);
	/* 置 0 使字符串为零填充 */
	result = (VarBit *) palloc0(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg),
		   Min(VARBITBYTES(result), VARBITBYTES(arg)));

	/*
	 * 必要时确保最后一字节零填充。若源数据短于
	 * 目标长度，这一步无用但安全（我们假定源数据
	 * 的最后一字节本身已被正确零填充）。
	 */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anybit_typmodin(ta, "bit"));
}

Datum
bittypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anybit_typmodout(typmod));
}


/*
 * varbit_in -
 *	  把字符串转换为位串的内部表示。
 *		与 bit_in 相同，只是 atttypmod 被当作最大
 *		长度而非强制位串达到的精确长度。
 */
Datum
varbit_in(PG_FUNCTION_ARGS)
{
	char	   *input_string = PG_GETARG_CSTRING(0);
#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	Node	   *escontext = fcinfo->context;
	VarBit	   *result;			/* 结果位串 */
	char	   *sp;				/* 指向字符串的指针 */
	bits8	   *r;				/* 指向结果 */
	int			len,			/* 整个数据结构的长度 */
				bitlen,			/* 位串中的位数 */
				slen;			/* 输入串的长度 */
	bool		bit_not_hex;	/* false=十六进制串  true=位串 */
	int			bc;
	bits8		x = 0;

	/* 检查首个字符是 b 还是 x */
	if (input_string[0] == 'b' || input_string[0] == 'B')
	{
		bit_not_hex = true;
		sp = input_string + 1;
	}
	else if (input_string[0] == 'x' || input_string[0] == 'X')
	{
		bit_not_hex = false;
		sp = input_string + 1;
	}
	else
	{
		bit_not_hex = true;
		sp = input_string;
	}

	/*
	 * 从输入字符串推定位长度。MaxAllocSize 保证常规
	 * 输入足够小，但须检查十六进制输入。
	 */
	slen = strlen(sp);
	if (bit_not_hex)
		bitlen = slen;
	else
	{
		if (slen > VARBITMAXLEN / 4)
			ereturn(escontext, (Datum) 0,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bit string length exceeds the maximum allowed (%d)",
							VARBITMAXLEN)));
		bitlen = slen * 4;
	}

	/*
	 * 有时未提供 atttypmod。若提供了它，就须确保
	 * 位串能装得下。
	 */
	if (atttypmod <= 0)
		atttypmod = bitlen;
	else if (bitlen > atttypmod)
		ereturn(escontext, (Datum) 0,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	/* 置 0 使 *r 总被初始化且字符串为零填充 */
	result = (VarBit *) palloc0(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = Min(bitlen, atttypmod);

	r = VARBITS(result);
	if (bit_not_hex)
	{
		/* 解析字符串的位表示 */
		/* 已知装得下，bitlen 与 atttypmod 比较过 */
		x = HIGHBIT;
		for (; *sp; sp++)
		{
			if (*sp == '1')
				*r |= x;
			else if (*sp != '0')
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid binary digit",
								pg_mblen(sp), sp)));

			x >>= 1;
			if (x == 0)
			{
				x = HIGHBIT;
				r++;
			}
		}
	}
	else
	{
		/* 解析字符串的十六进制表示 */
		for (bc = 0; *sp; sp++)
		{
			if (*sp >= '0' && *sp <= '9')
				x = (bits8) (*sp - '0');
			else if (*sp >= 'A' && *sp <= 'F')
				x = (bits8) (*sp - 'A') + 10;
			else if (*sp >= 'a' && *sp <= 'f')
				x = (bits8) (*sp - 'a') + 10;
			else
				ereturn(escontext, (Datum) 0,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("\"%.*s\" is not a valid hexadecimal digit",
								pg_mblen(sp), sp)));

			if (bc)
			{
				*r++ |= x;
				bc = 0;
			}
			else
			{
				*r = x << 4;
				bc = 1;
			}
		}
	}

	PG_RETURN_VARBIT_P(result);
}

/*
 * varbit_out -
 *	  以位形式打印字符串以准确保留长度
 *
 * XXX varbit_recv() 与 varbit_in() 的十六进制输入能
 * 加载函数无法输出的值，宜用 hex 输出
 */
Datum
varbit_out(PG_FUNCTION_ARGS)
{
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	char	   *result,
			   *r;
	bits8	   *sp;
	bits8		x;
	int			i,
				k,
				len;

	/* 断言：帮助捕获任何未正确填充的位函数 */
	VARBIT_CORRECTLY_PADDED(s);

	len = VARBITLEN(s);
	result = (char *) palloc(len + 1);
	sp = VARBITS(s);
	r = result;
	for (i = 0; i <= len - BITS_PER_BYTE; i += BITS_PER_BYTE, sp++)
	{
		/*打印整字节*/
		x = *sp;
		for (k = 0; k < BITS_PER_BYTE; k++)
		{
			*r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
			x <<= 1;
		}
	}
	if (i < len)
	{
		/* 打印残余字节 */
		x = *sp;
		for (k = i; k < len; k++)
		{
			*r++ = IS_HIGHBIT_SET(x) ? '1' : '0';
			x <<= 1;
		}
	}
	*r = '\0';

	PG_RETURN_CSTRING(result);
}

/*
 *		varbit_recv - 把外部二进制格式转换为 varbit
 *
 * 外部格式为 int32 的 bitlen，随后是字节数组。
 */
Datum
varbit_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

#ifdef NOT_USED
	Oid			typelem = PG_GETARG_OID(1);
#endif
	int32		atttypmod = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;

	bitlen = pq_getmsgint(buf, sizeof(int32));
	if (bitlen < 0 || bitlen > VARBITMAXLEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("invalid length in external bit string")));

	/*
	 * 有时未提供 atttypmod。若提供了它，就须确保
	 * 位串能装得下。
	 */
	if (atttypmod > 0 && bitlen > atttypmod)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						atttypmod)));

	len = VARBITTOTALLEN(bitlen);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	pq_copymsgbytes(buf, (char *) VARBITS(result), VARBITBYTES(result));

	/* 确保最后一字节零填充正确 */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

/*
 *		varbit_send - 将 varbit 转为二进制格式
 */
Datum
varbit_send(PG_FUNCTION_ARGS)
{
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, VARBITLEN(s));
	pq_sendbytes(&buf, VARBITS(s), VARBITBYTES(s));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * varbit_support()
 *
 * varbit() 长度强转函数的规划器支持函数。
 *
 * 目前我们唯一能做的有意思的事，是把满足
 * 新最大长度 >= 旧最大长度这一条件的调用展平。
 * isExplicit 参数可忽略，因为它只影响截断情形。
 */
Datum
varbit_support(PG_FUNCTION_ARGS)
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
			int32		new_typmod = DatumGetInt32(((Const *) typmod)->constvalue);
			int32		old_max = exprTypmod(source);
			int32		new_max = new_typmod;

			/* 注：varbit() 以 typmod 0 为无效，我们亦然 */
			if (new_max <= 0 || (old_max > 0 && old_max <= new_max))
				ret = relabel_to_typmod(source, new_typmod);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * varbit()
 * 把 varbit() 类型转换为特定内部长度。
 * len 是列定义中指定的最大位长度。
 *
 * 隐式转换时，若源数据过长则报错。
 * 显式转换时，静默截断到最大长度。
 */
Datum
varbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		len = PG_GETARG_INT32(1);
	bool		isExplicit = PG_GETARG_BOOL(2);
	VarBit	   *result;
	int			rlen;

	/* typmod 无效或给定数据已与其匹配时无须处理 */
	if (len <= 0 || len >= VARBITLEN(arg))
		PG_RETURN_VARBIT_P(arg);

	if (!isExplicit)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("bit string too long for type bit varying(%d)",
						len)));

	rlen = VARBITTOTALLEN(len);
	result = (VarBit *) palloc(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = len;

	memcpy(VARBITS(result), VARBITS(arg), VARBITBYTES(result));

	/* 确保最后一字节零填充正确 */
	VARBIT_PAD(result);

	PG_RETURN_VARBIT_P(result);
}

Datum
varbittypmodin(PG_FUNCTION_ARGS)
{
	ArrayType  *ta = PG_GETARG_ARRAYTYPE_P(0);

	PG_RETURN_INT32(anybit_typmodin(ta, "varbit"));
}

Datum
varbittypmodout(PG_FUNCTION_ARGS)
{
	int32		typmod = PG_GETARG_INT32(0);

	PG_RETURN_CSTRING(anybit_typmodout(typmod));
}


/*
 * 比较算子
 *
 * 位串只需一组比较算子，因为零填充位串与
 * 变长位串的长度存储方式相同。
 *
 * 注意标准对零填充位串与变长位串之间的比较
 * 并未给出无歧义的规定。若把同一个值写入零
 * 填充位串和变长位串，而零填充位串的长度
 * 更大，则它会更大。
 *
 * 位串开头处的零无法被简单地忽略，因为它们
 * 可能是位串的一部分且可能有意义。
 *
 * 注意：btree 索引要求这些例程不泄漏内存；因此，
 * 务必当心释放 toast 值的工作副本。大多数地方
 * 无须如此当心。
 */

/*
 * bit_cmp
 *
 * 比较两个位串，按第一个串小于、等于或大于
 * 第二个串分别返回 <0、0、>0。所有位都参与比较，
 * 且额外的零位可能使一个串比另一个更小或更大，
 * 即使二者零填充后的值相同。
 */
static int32
bit_cmp(VarBit *arg1, VarBit *arg2)
{
	int			bitlen1,
				bytelen1,
				bitlen2,
				bytelen2;
	int32		cmp;

	bytelen1 = VARBITBYTES(arg1);
	bytelen2 = VARBITBYTES(arg2);

	cmp = memcmp(VARBITS(arg1), VARBITS(arg2), Min(bytelen1, bytelen2));
	if (cmp == 0)
	{
		bitlen1 = VARBITLEN(arg1);
		bitlen2 = VARBITLEN(arg2);
		if (bitlen1 != bitlen2)
			cmp = (bitlen1 < bitlen2) ? -1 : 1;
	}
	return cmp;
}

Datum
biteq(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;
	int			bitlen1,
				bitlen2;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	/* 不同长度的快速路径 */
	if (bitlen1 != bitlen2)
		result = false;
	else
		result = (bit_cmp(arg1, arg2) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitne(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;
	int			bitlen1,
				bitlen2;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	/* 不同长度的快速路径 */
	if (bitlen1 != bitlen2)
		result = true;
	else
		result = (bit_cmp(arg1, arg2) != 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitlt(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) < 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitle(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) <= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitgt(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) > 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitge(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	bool		result;

	result = (bit_cmp(arg1, arg2) >= 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

Datum
bitcmp(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	int32		result;

	result = bit_cmp(arg1, arg2);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

/*
 * bitcat
 * 位串的连接
 */
Datum
bitcat(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);

	PG_RETURN_VARBIT_P(bit_catenate(arg1, arg2));
}

static VarBit *
bit_catenate(VarBit *arg1, VarBit *arg2)
{
	VarBit	   *result;
	int			bitlen1,
				bitlen2,
				bytelen,
				bit1pad,
				bit2shift;
	bits8	   *pr,
			   *pa;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);

	if (bitlen1 > VARBITMAXLEN - bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("bit string length exceeds the maximum allowed (%d)",
						VARBITMAXLEN)));
	bytelen = VARBITTOTALLEN(bitlen1 + bitlen2);

	result = (VarBit *) palloc(bytelen);
	SET_VARSIZE(result, bytelen);
	VARBITLEN(result) = bitlen1 + bitlen2;

	/* 复制第一个位串 */
	memcpy(VARBITS(result), VARBITS(arg1), VARBITBYTES(arg1));

	/* 复制第二个位串 */
	bit1pad = VARBITPAD(arg1);
	if (bit1pad == 0)
	{
		memcpy(VARBITS(result) + VARBITBYTES(arg1), VARBITS(arg2),
			   VARBITBYTES(arg2));
	}
	else if (bitlen2 > 0)
	{
		/* 须移位全部位以对齐 */
		bit2shift = BITS_PER_BYTE - bit1pad;
		pr = VARBITS(result) + VARBITBYTES(arg1) - 1;
		for (pa = VARBITS(arg2); pa < VARBITEND(arg2); pa++)
		{
			*pr |= ((*pa >> bit2shift) & BITMASK);
			pr++;
			if (pr < VARBITEND(result))
				*pr = (*pa << bit1pad) & BITMASK;
		}
	}

	/* 此时填充位应当已经为零 */

	return result;
}

/*
 * bitsubstr
 * 从位串中取子串。
 * 注：s 从 1 起
 * SQL draft 6.10 9)
 */
Datum
bitsubstr(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(bitsubstring(PG_GETARG_VARBIT_P(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

Datum
bitsubstr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(bitsubstring(PG_GETARG_VARBIT_P(0),
									PG_GETARG_INT32(1),
									-1, true));
}

static VarBit *
bitsubstring(VarBit *arg, int32 s, int32 l, bool length_not_specified)
{
	VarBit	   *result;
	int			bitlen,
				rbitlen,
				len,
				ishift,
				i;
	int32		e,
				s1,
				e1;
	bits8	   *r,
			   *ps;

	bitlen = VARBITLEN(arg);
	s1 = Max(s, 1);
	/* 若无上界，则用字符串末尾 */
	if (length_not_specified)
	{
		e1 = bitlen + 1;
	}
	else if (l < 0)
	{
		/* SQL99 规定 E < S（即长度为负）时报错 */
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
		e1 = -1;				/* 安抚笨编译器 */
	}
	else if (pg_add_s32_overflow(s, l, &e))
	{
		/*
		 * L 可能大到使 S + L 溢出，此时子串必须延伸到
		 * 字符串末尾。
		 */
		e1 = bitlen + 1;
	}
	else
	{
		e1 = Min(e, bitlen + 1);
	}
	if (s1 > bitlen || e1 <= s1)
	{
		/* 须返回零长度位串 */
		len = VARBITTOTALLEN(0);
		result = (VarBit *) palloc(len);
		SET_VARSIZE(result, len);
		VARBITLEN(result) = 0;
	}
	else
	{
		/*
		 * 好，我们得到了真正的子串，从位置 s1-1 开始
		 * 到 e1-1 结束
		 */
		rbitlen = e1 - s1;
		len = VARBITTOTALLEN(rbitlen);
		result = (VarBit *) palloc(len);
		SET_VARSIZE(result, len);
		VARBITLEN(result) = rbitlen;
		len -= VARHDRSZ + VARBITHDRSZ;
		/* 是否从字节边界复制？ */
		if ((s1 - 1) % BITS_PER_BYTE == 0)
		{
			/* 是，复制整字节 */
			memcpy(VARBITS(result), VARBITS(arg) + (s1 - 1) / BITS_PER_BYTE,
				   len);
		}
		else
		{
			/* 算出序列需要移位多少 */
			ishift = (s1 - 1) % BITS_PER_BYTE;
			r = VARBITS(result);
			ps = VARBITS(arg) + (s1 - 1) / BITS_PER_BYTE;
			for (i = 0; i < len; i++)
			{
				*r = (*ps << ishift) & BITMASK;
				if ((++ps) < VARBITEND(arg))
					*r |= *ps >> (BITS_PER_BYTE - ishift);
				r++;
			}
		}

		/* 确保最后一字节零填充正确 */
		VARBIT_PAD(result);
	}

	return result;
}

/*
 * bitoverlay
 *	用第二个串替换第一个串中指定的子串
 *
 * SQL 标准用子串与连接来定义 OVERLAY()。
 * 此代码是对标准所述内容的直接实现。
 */
Datum
bitoverlay(PG_FUNCTION_ARGS)
{
	VarBit	   *t1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *t2 = PG_GETARG_VARBIT_P(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl = PG_GETARG_INT32(3);	/* 子串长度 */

	PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

Datum
bitoverlay_no_len(PG_FUNCTION_ARGS)
{
	VarBit	   *t1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *t2 = PG_GETARG_VARBIT_P(1);
	int			sp = PG_GETARG_INT32(2);	/* 子串起始位置 */
	int			sl;

	sl = VARBITLEN(t2);			/* 即 length(t2) */
	PG_RETURN_VARBIT_P(bit_overlay(t1, t2, sp, sl));
}

static VarBit *
bit_overlay(VarBit *t1, VarBit *t2, int sp, int sl)
{
	VarBit	   *result;
	VarBit	   *s1;
	VarBit	   *s2;
	int			sp_pl_sl;

	/*
	 * 检查可能的整数溢出情形。对负的 sp，抛出
	 * "子串长度"错误，因为按规范对 OVERLAY()
	 * 定义，这正是应有的行为。
	 */
	if (sp <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_SUBSTRING_ERROR),
				 errmsg("negative substring length not allowed")));
	if (pg_add_s32_overflow(sp, sl, &sp_pl_sl))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	s1 = bitsubstring(t1, 1, sp - 1, false);
	s2 = bitsubstring(t1, sp_pl_sl, -1, true);
	result = bit_catenate(s1, t2);
	result = bit_catenate(result, s2);

	return result;
}

/*
 * bit_count
 *
 * 返回位串中置位的位数。
 */
Datum
bit_bit_count(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT64(pg_popcount((char *) VARBITS(arg), VARBITBYTES(arg)));
}

/*
 * bitlength, bitoctetlength
 * 返回位串的长度
 */
Datum
bitlength(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT32(VARBITLEN(arg));
}

Datum
bitoctetlength(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);

	PG_RETURN_INT32(VARBITBYTES(arg));
}

/*
 * bit_and
 * 对两个位串执行逻辑 AND。
 */
Datum
bit_and(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot AND bit strings of different sizes")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ & *p2++;

	/* 无须填充，0 填充位相与仍为 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bit_or
 * 对两个位串执行逻辑 OR。
 */
Datum
bit_or(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot OR bit strings of different sizes")));
	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ | *p2++;

	/* 无须填充，0 填充位相或仍为 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitxor
 * 对两个位串执行逻辑 XOR。
 */
Datum
bitxor(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	VarBit	   *arg2 = PG_GETARG_VARBIT_P(1);
	VarBit	   *result;
	int			len,
				bitlen1,
				bitlen2,
				i;
	bits8	   *p1,
			   *p2,
			   *r;

	bitlen1 = VARBITLEN(arg1);
	bitlen2 = VARBITLEN(arg2);
	if (bitlen1 != bitlen2)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_LENGTH_MISMATCH),
				 errmsg("cannot XOR bit strings of different sizes")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen1;

	p1 = VARBITS(arg1);
	p2 = VARBITS(arg2);
	r = VARBITS(result);
	for (i = 0; i < VARBITBYTES(arg1); i++)
		*r++ = *p1++ ^ *p2++;

	/* 无须填充，0 填充位异或仍为 0 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitnot
 * 对一个位串执行逻辑 NOT。
 */
Datum
bitnot(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	VarBit	   *result;
	bits8	   *p,
			   *r;

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
	VARBITLEN(result) = VARBITLEN(arg);

	p = VARBITS(arg);
	r = VARBITS(result);
	for (; p < VARBITEND(arg); p++)
		*r++ = ~*p;

	/* 必须将结果零填充，因为此处多余位必为 1 */
	VARBIT_PAD_LAST(result, r);

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftleft
 * 执行左移（即朝字符串开头方向）
 */
Datum
bitshiftleft(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		shft = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			byte_shift,
				ishift,
				len;
	bits8	   *p,
			   *r;

	/* 负移位是向右移位 */
	if (shft < 0)
	{
		/* 防止取负时整数溢出 */
		if (shft < -VARBITMAXLEN)
			shft = -VARBITMAXLEN;
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftright,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));
	}

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
	VARBITLEN(result) = VARBITLEN(arg);
	r = VARBITS(result);

	/* 若所有位都被移出，则返回全零串 */
	if (shft >= VARBITLEN(arg))
	{
		MemSet(r, 0, VARBITBYTES(arg));
		PG_RETURN_VARBIT_P(result);
	}

	byte_shift = shft / BITS_PER_BYTE;
	ishift = shft % BITS_PER_BYTE;
	p = VARBITS(arg) + byte_shift;

	if (ishift == 0)
	{
		/* 特例：可以用 memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r, p, len);
		MemSet(r + len, 0, byte_shift);
	}
	else
	{
		for (; p < VARBITEND(arg); r++)
		{
			*r = *p << ishift;
			if ((++p) < VARBITEND(arg))
				*r |= *p >> (BITS_PER_BYTE - ishift);
		}
		for (; r < VARBITEND(result); r++)
			*r = 0;
	}

	/* 此时填充位应当已经为零 */

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitshiftright
 * 执行右移（即朝字符串末尾方向）
 */
Datum
bitshiftright(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	int32		shft = PG_GETARG_INT32(1);
	VarBit	   *result;
	int			byte_shift,
				ishift,
				len;
	bits8	   *p,
			   *r;

	/* 负移位是向左移位 */
	if (shft < 0)
	{
		/* 防止取负时整数溢出 */
		if (shft < -VARBITMAXLEN)
			shft = -VARBITMAXLEN;
		PG_RETURN_DATUM(DirectFunctionCall2(bitshiftleft,
											VarBitPGetDatum(arg),
											Int32GetDatum(-shft)));
	}

	result = (VarBit *) palloc(VARSIZE(arg));
	SET_VARSIZE(result, VARSIZE(arg));
	VARBITLEN(result) = VARBITLEN(arg);
	r = VARBITS(result);

	/* 若所有位都被移出，则返回全零串 */
	if (shft >= VARBITLEN(arg))
	{
		MemSet(r, 0, VARBITBYTES(arg));
		PG_RETURN_VARBIT_P(result);
	}

	byte_shift = shft / BITS_PER_BYTE;
	ishift = shft % BITS_PER_BYTE;
	p = VARBITS(arg);

	/* 把结果的第一部分置 0 */
	MemSet(r, 0, byte_shift);
	r += byte_shift;

	if (ishift == 0)
	{
		/* 特例：可以用 memcpy */
		len = VARBITBYTES(arg) - byte_shift;
		memcpy(r, p, len);
		r += len;
	}
	else
	{
		if (r < VARBITEND(result))
			*r = 0;				/* 首字节置0 */
		for (; r < VARBITEND(result); p++)
		{
			*r |= *p >> ishift;
			if ((++r) < VARBITEND(result))
				*r = (*p << (BITS_PER_BYTE - ishift)) & BITMASK;
		}
	}

	/* 可能已把 1 移入填充位，所以要修正 */
	VARBIT_PAD_LAST(result, r);

	PG_RETURN_VARBIT_P(result);
}

/*
 * 任何标准都未定义此函数。我们在这里保留位
 * 的自然顺序，因为它更直观。
 */
Datum
bitfromint4(PG_FUNCTION_ARGS)
{
	int32		a = PG_GETARG_INT32(0);
	int32		typmod = PG_GETARG_INT32(1);
	VarBit	   *result;
	bits8	   *r;
	int			rlen;
	int			destbitsleft,
				srcbitsleft;

	if (typmod <= 0 || typmod > VARBITMAXLEN)
		typmod = 1;				/* 默认位长 */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = typmod;

	r = VARBITS(result);
	destbitsleft = typmod;
	srcbitsleft = 32;
	/* 丢弃装不下的输入位 */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* 对多余字节做符号填充 */
	while (destbitsleft >= srcbitsleft + 8)
	{
		*r++ = (bits8) ((a < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* 存首小数字节 */
	if (destbitsleft > srcbitsleft)
	{
		unsigned int val = (unsigned int) (a >> (destbitsleft - 8));

		/* 强制符号填充，以防编译器把 >> 实现为补零 */
		if (a < 0)
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
		*r++ = (bits8) (val & BITMASK);
		destbitsleft -= 8;
	}
	/* srcbitsleft 与 destbitsleft 此时相同，无须都跟踪 */
	/* 存整字节 */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((a >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* 存末小数字节 */
	if (destbitsleft > 0)
		*r = (bits8) ((a << (8 - destbitsleft)) & BITMASK);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittoint4(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	uint32		result;
	bits8	   *r;

	/* 检查位串是否过长 */
	if (VARBITLEN(arg) > sizeof(result) * BITS_PER_BYTE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	result = 0;
	for (r = VARBITS(arg); r < VARBITEND(arg); r++)
	{
		result <<= BITS_PER_BYTE;
		result |= *r;
	}
	/* 现在将结果移位以顾及末端的填充 */
	result >>= VARBITPAD(arg);

	PG_RETURN_INT32(result);
}

Datum
bitfromint8(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int32		typmod = PG_GETARG_INT32(1);
	VarBit	   *result;
	bits8	   *r;
	int			rlen;
	int			destbitsleft,
				srcbitsleft;

	if (typmod <= 0 || typmod > VARBITMAXLEN)
		typmod = 1;				/* 默认位长 */

	rlen = VARBITTOTALLEN(typmod);
	result = (VarBit *) palloc(rlen);
	SET_VARSIZE(result, rlen);
	VARBITLEN(result) = typmod;

	r = VARBITS(result);
	destbitsleft = typmod;
	srcbitsleft = 64;
	/* 丢弃装不下的输入位 */
	srcbitsleft = Min(srcbitsleft, destbitsleft);
	/* 对多余字节做符号填充 */
	while (destbitsleft >= srcbitsleft + 8)
	{
		*r++ = (bits8) ((a < 0) ? BITMASK : 0);
		destbitsleft -= 8;
	}
	/* 存首小数字节 */
	if (destbitsleft > srcbitsleft)
	{
		unsigned int val = (unsigned int) (a >> (destbitsleft - 8));

		/* 强制符号填充，以防编译器把 >> 实现为补零 */
		if (a < 0)
			val |= ((unsigned int) -1) << (srcbitsleft + 8 - destbitsleft);
		*r++ = (bits8) (val & BITMASK);
		destbitsleft -= 8;
	}
	/* srcbitsleft 与 destbitsleft 此时相同，无须都跟踪 */
	/* 存整字节 */
	while (destbitsleft >= 8)
	{
		*r++ = (bits8) ((a >> (destbitsleft - 8)) & BITMASK);
		destbitsleft -= 8;
	}
	/* 存末小数字节 */
	if (destbitsleft > 0)
		*r = (bits8) ((a << (8 - destbitsleft)) & BITMASK);

	PG_RETURN_VARBIT_P(result);
}

Datum
bittoint8(PG_FUNCTION_ARGS)
{
	VarBit	   *arg = PG_GETARG_VARBIT_P(0);
	uint64		result;
	bits8	   *r;

	/* 检查位串是否过长 */
	if (VARBITLEN(arg) > sizeof(result) * BITS_PER_BYTE)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	result = 0;
	for (r = VARBITS(arg); r < VARBITEND(arg); r++)
	{
		result <<= BITS_PER_BYTE;
		result |= *r;
	}
	/* 现在将结果移位以顾及末端的填充 */
	result >>= VARBITPAD(arg);

	PG_RETURN_INT64(result);
}


/*
 * 确定 S2 在位串 S1 中的位置（串从 1 起算）。
 * 若 S2 未出现在 S1 中，本函数返回 0。
 * 若 S2 长度为 0，本函数返回 1。
 * 用法与其他数据类型的 POSITION() 函数兼容。
 */
Datum
bitposition(PG_FUNCTION_ARGS)
{
	VarBit	   *str = PG_GETARG_VARBIT_P(0);
	VarBit	   *substr = PG_GETARG_VARBIT_P(1);
	int			substr_length,
				str_length,
				i,
				is;
	bits8	   *s,				/* 子串内指针 */
			   *p;				/*串内指针*/
	bits8		cmp,			/* 移位后待比子串字节 */
				mask1,			/* 子串字节右移的掩码 */
				mask2,			/* 子串字节左移的掩码 */
				end_mask,		/* 末子串字节之掩码 */
				str_mask;		/* 末串字节之掩码 */
	bool		is_match;

	/* 取子串长度 */
	substr_length = VARBITLEN(substr);
	str_length = VARBITLEN(str);

	/* 串长为零或子串长于串时返回 0 */
	if ((str_length == 0) || (substr_length > str_length))
		PG_RETURN_INT32(0);

	/* 零长度子串意味着返回 1 */
	if (substr_length == 0)
		PG_RETURN_INT32(1);

	/* 初始化填充掩码 */
	end_mask = BITMASK << VARBITPAD(substr);
	str_mask = BITMASK << VARBITPAD(str);
	for (i = 0; i < VARBITBYTES(str) - VARBITBYTES(substr) + 1; i++)
	{
		for (is = 0; is < BITS_PER_BYTE; is++)
		{
			is_match = true;
			p = VARBITS(str) + i;
			mask1 = BITMASK >> is;
			mask2 = ~mask1;
			for (s = VARBITS(substr);
				 is_match && s < VARBITEND(substr); s++)
			{
				cmp = *s >> is;
				if (s == VARBITEND(substr) - 1)
				{
					mask1 &= end_mask >> is;
					if (p == VARBITEND(str) - 1)
					{
						/* 检查 str 剩余部分是否足够 */
						if (mask1 & ~str_mask)
						{
							is_match = false;
							break;
						}
						mask1 &= str_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask1) == 0;
				if (!is_match)
					break;
				/* 前进到下一字节 */
				p++;
				if (p == VARBITEND(str))
				{
					mask2 = end_mask << (BITS_PER_BYTE - is);
					is_match = mask2 == 0;
#if 0
					elog(DEBUG4, "S. %d %d em=%2x sm=%2x r=%d",
						 i, is, end_mask, mask2, is_match);
#endif
					break;
				}
				cmp = *s << (BITS_PER_BYTE - is);
				if (s == VARBITEND(substr) - 1)
				{
					mask2 &= end_mask << (BITS_PER_BYTE - is);
					if (p == VARBITEND(str) - 1)
					{
						if (mask2 & ~str_mask)
						{
							is_match = false;
							break;
						}
						mask2 &= str_mask;
					}
				}
				is_match = ((cmp ^ *p) & mask2) == 0;
			}
			/* 找到匹配？ */
			if (is_match)
				PG_RETURN_INT32(i * BITS_PER_BYTE + is + 1);
		}
	}
	PG_RETURN_INT32(0);
}


/*
 * bitsetbit
 *
 * 给定一个 'bit' 类型实例，创建一个把第
 * N 位置为给定值的新实例。
 *
 * 位位置的指定方式是从左到右、从零开始，
 * 与其他 get_bit 和 set_bit 函数一致，但与
 * 标准的 substring、position、overlay 函数不一致。
 */
Datum
bitsetbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	int32		n = PG_GETARG_INT32(1);
	int32		newBit = PG_GETARG_INT32(2);
	VarBit	   *result;
	int			len,
				bitlen;
	bits8	   *r,
			   *p;
	int			byteNo,
				bitNo;

	bitlen = VARBITLEN(arg1);
	if (n < 0 || n >= bitlen)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("bit index %d out of valid range (0..%d)",
						n, bitlen - 1)));

	/*
	 * 健全检查!
	 */
	if (newBit != 0 && newBit != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("new bit must be 0 or 1")));

	len = VARSIZE(arg1);
	result = (VarBit *) palloc(len);
	SET_VARSIZE(result, len);
	VARBITLEN(result) = bitlen;

	p = VARBITS(arg1);
	r = VARBITS(result);

	memcpy(r, p, VARBITBYTES(arg1));

	byteNo = n / BITS_PER_BYTE;
	bitNo = BITS_PER_BYTE - 1 - (n % BITS_PER_BYTE);

	/*
	 * 更新字节。
	 */
	if (newBit == 0)
		r[byteNo] &= (~(1 << bitNo));
	else
		r[byteNo] |= (1 << bitNo);

	PG_RETURN_VARBIT_P(result);
}

/*
 * bitgetbit
 *
 * 返回位数组第 N 位的值（0 或 1）。
 *
 * 位位置的指定方式是从左到右、从零开始，
 * 与其他 get_bit 和 set_bit 函数一致，但与
 * 标准的 substring、position、overlay 函数不一致。
 */
Datum
bitgetbit(PG_FUNCTION_ARGS)
{
	VarBit	   *arg1 = PG_GETARG_VARBIT_P(0);
	int32		n = PG_GETARG_INT32(1);
	int			bitlen;
	bits8	   *p;
	int			byteNo,
				bitNo;

	bitlen = VARBITLEN(arg1);
	if (n < 0 || n >= bitlen)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("bit index %d out of valid range (0..%d)",
						n, bitlen - 1)));

	p = VARBITS(arg1);

	byteNo = n / BITS_PER_BYTE;
	bitNo = BITS_PER_BYTE - 1 - (n % BITS_PER_BYTE);

	if (p[byteNo] & (1 << bitNo))
		PG_RETURN_INT32(1);
	else
		PG_RETURN_INT32(0);
}
